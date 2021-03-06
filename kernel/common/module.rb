# depends on: class.rb proc.rb autoload.rb

##
# Some terminology notes:
#
# [Encloser] The Class or Module inside which this one is defined or, in the
#            event we are at top-level, Object.
#
# [Direct superclass] Whatever is next in the chain of superclass invocations.
#                     This may be either an included Module, a Class or nil.
#
# [Superclass] The real semantic superclass and thus only applies to Class
#              objects.

class Module

  def constants_table() ; @constants ; end

  # I (Evan) don't like either of these. I think they could easily
  # break things. We should check on that.
  def constants_table=(c) ; @constants = c    ; end
  def method_table=(m)    ; @method_table = m ; end

  def self.nesting
    mod  = MethodContext.current.sender.receiver
    unless mod.kind_of? Module
      mod = MethodContext.current.sender.method_module
    end
    nesting = []
    while mod != Object && mod.kind_of?(Module)
      nesting << mod
      mod = mod.encloser
    end
    nesting
  end

  def initialize(&block)
    @method_table = MethodTable.new
    @constants = LookupTable.new

    _eval_under(self, &block) if block
  end

  def verify_class_variable_name(name)
    name = name.kind_of?(Symbol) ? name.to_s : StringValue(name)
    unless name[0..1] == '@@' and name[2].toupper.between?(?A, ?Z) or name[2] == ?_
      raise NameError, "#{name} is not an allowed class variable name"
    end
    name.to_sym
  end
  private :verify_class_variable_name

  def class_variables_table
    @class_variables ||= Hash.new
  end
  private :class_variables_table

  def class_variable_set(name, val)
    name = verify_class_variable_name name

    current = direct_superclass
    while current
      if current.__kind_of__ MetaClass
        vars = current.attached_instance.send :class_variables_table
      elsif current.__kind_of__ IncludedModule
        vars = current.module.send :class_variables_table
      else
        vars = current.send :class_variables_table
      end
      return vars[name] = val if vars.key? name
      current = current.direct_superclass
    end

    if self.__kind_of__ MetaClass
      table = self.attached_instance.send :class_variables_table
    else
      table = class_variables_table
    end
    table[name] = val
  end

  def class_variable_get(name)
    name = verify_class_variable_name name

    current = self
    while current
      if current.__kind_of__ MetaClass
        vars = current.attached_instance.send :class_variables_table
      elsif current.__kind_of__ IncludedModule
        vars = current.module.send :class_variables_table
      else
        vars = current.send :class_variables_table
      end
      return vars[name] if vars.key? name
      current = current.direct_superclass
    end

    # Try to print something useful for anonymous modules and metaclasses
    module_name = self.name || self.inspect
    raise NameError, "uninitialized class variable #{name} in #{module_name}"
  end

  def class_variable_defined?(name)
    name = verify_class_variable_name name

    current = self
    while current
      if current.__kind_of__ IncludedModule
        vars = current.module.send :class_variables_table
      else
        vars = current.send :class_variables_table
      end
      return true if vars.key? name
      current = current.direct_superclass
    end
    return false
  end

  def class_variables(symbols = false)
    names = []
    ancestors.map do |mod|
      names.concat mod.send(:class_variables_table).keys
    end
    names = names.map { |name| name.to_s } unless symbols
    names
  end

  def name
    @name ? @name.to_s : ""
  end

  def to_s
    @name ? @name.to_s : super
  end

  alias_method :inspect, :to_s

  def find_method_in_hierarchy(sym)
    mod = self

    while mod
      if method = mod.method_table[sym.to_sym]
        return method
      end

      mod = mod.direct_superclass
    end

    # Always also search Object (and everything included in Object).
    # This lets a module alias methods on Kernel.
    if instance_of?(Module) and self != Kernel
      return Object.find_method_in_hierarchy(sym)
    end
  end

  def ancestors
    if self.class == MetaClass
      out = []
    else
      out = [self]
    end
    sup = direct_superclass()
    while sup
      if sup.class == IncludedModule
        out << sup.module
      elsif sup.class != MetaClass
        out << sup
      end
      sup = sup.direct_superclass()
    end
    return out
  end

  def superclass_chain
    out = []
    mod = direct_superclass()
    while mod
      out << mod
      mod = mod.direct_superclass()
    end

    return out
  end

  # Create a wrapper to a function in a C-linked library that
  # exists somewhere in the system. If a specific library is
  # not given, the function is assumed to exist in the running
  # process, the Rubinius executable. The process contains many
  # linked libraries in addition to Rubinius' codebase, libc of
  # course the most prominent on the system side. The wrapper
  # method is added to the Module as a singleton method or a
  # "class method."
  #
  # The function is specified like a declaration: the first
  # argument is the type symbol for the return type (see FFI
  # documentation for types), the second argument is the name
  # of the function and the third argument is an Array of the
  # types of the function's arguments. Currently at most 6
  # arguments can be given.
  #
  #   # If you want to wrap this function:
  #   int foobar(double arg_one, const char* some_string);
  #
  #   # The arguments to #attach_foreign look like this:
  #   :int, 'foobar', [:double, :string]
  #
  # If the function is from an external library such as, say,
  # libpcre, libcurl etc. you can give the name or path of
  # the library. The fourth argument is an option hash and
  # the library name should be given in the +:from+ key of
  # the hash. The name may (and for portable code, should)
  # omit the file extension. If the extension is present,
  # it must be the correct one for the runtime platform.
  # The library is searched for in the system library paths
  # but if necessary, the full absolute or relative path can
  # be given.
  #
  # By default, the new method's name is the same as the
  # function it wraps but in some cases it is desirable to
  # change this. You can specify the method name in the +:as+
  # key of the option hash.
  def attach_foreign(ret_type, name, arg_types, opts = {})
    lib = opts[:from]

    if lib and !lib.chomp! ".#{Rubinius::LIBSUFFIX}"
      lib.chomp! ".#{Rubinius::ALT_LIBSUFFIX}" rescue nil     # .defined? is broken anyway
    end

    func = FFI.create_function lib, name.to_s, arg_types, ret_type
    metaclass.method_table[(opts[:as] || name).to_sym] = func
  end

  def find_class_method_in_hierarchy(sym)
    self.metaclass.find_method_in_hierarchy(sym)
  end

  def remote_alias(new_name, mod, current_name)
    cm = mod.find_method_in_hierarchy(current_name)
    unless cm
      raise NameError, "Unable to find method '#{current_name}' under #{mod}"
    end

    if cm.kind_of? Tuple
      meth = cm[1]
    else
      meth = cm
    end

    if meth.primitive and meth.primitive > 0
      raise NameError, "Unable to remote alias primitive method '#{current_name}'"
    end

    method_table[new_name] = cm
    Rubinius::VM.reset_method_cache(new_name)

    return new_name
  end

  def undef_method(*names)
    names.each do |name|
      name = Type.coerce_to_symbol(name)
      # Will raise a NameError if the method doesn't exist.
      instance_method(name)
      method_table[name] = false
      Rubinius::VM.reset_method_cache(name)

      method_undefined(name) if respond_to? :method_undefined
    end

    nil
  end

  def remove_method(*names)
    names.each do |name|
      name = Type.coerce_to_symbol(name)
      # Will raise a NameError if the method doesn't exist.
      instance_method(name)
      unless self.method_table[name]
        raise NameError, "method `#{name}' not defined in #{self.name}"
      end
      method_table.delete name
      Rubinius::VM.reset_method_cache(name)

      method_removed(name) if respond_to? :method_removed
    end

    nil
  end

  def public_method_defined?(sym)
    sym = StringValue(sym) unless sym.is_a? Symbol
    m = find_method_in_hierarchy sym
    m &&= Tuple[:public, m] unless m.is_a? Tuple
    m ? m.first == :public : false
  end

  def private_method_defined?(sym)
    sym = StringValue(sym) unless sym.is_a? Symbol
    m = find_method_in_hierarchy sym
    m &&= Tuple[:public, m] unless m.is_a? Tuple
    m ? m.first == :private : false
  end

  def protected_method_defined?(sym)
    sym = StringValue(sym) unless sym.is_a? Symbol
    m = find_method_in_hierarchy sym
    m &&= Tuple[:public, m] unless m.is_a? Tuple
    m ? m.first == :protected : false
  end

  def method_defined?(sym)
    sym = Type.coerce_to_symbol(sym)
    m = find_method_in_hierarchy sym
    m &&= Tuple[:public, m] unless m.is_a? Tuple
    m ? [:public,:protected].include?(m.first) : false
  end

  ##
  # Returns an UnboundMethod corresponding to the given name. The name will be
  # searched for in this Module as well as any included Modules or
  # superclasses. The UnboundMethod is populated with the method name and the
  # Module that the method was located in.
  #
  # Raises a TypeError if the given name.to_sym fails and a NameError if the
  # name cannot be located.

  def instance_method(name)
    name = Type.coerce_to name, Symbol, :to_sym

    mod = self
    cmethod = @method_table[name]

    while mod and cmethod.nil? do
      mod = mod.direct_superclass
      cmethod = mod.method_table[name]
    end

    # unwrap the real method from Visibility if needed
    cmethod = cmethod.method if cmethod.kind_of? CompiledMethod::Visibility

    # We want to show the real module
    mod = mod.module if mod.class == IncludedModule
    return UnboundMethod.new(mod, cmethod, self) if cmethod

    raise NameError, "Undefined method `#{name}' for #{self}"
  end

  def instance_methods(all=true)
    filter_methods(:public_names, all) | filter_methods(:protected_names, all)
  end

  def public_instance_methods(all=true)
    filter_methods(:public_names, all)
  end

  def private_instance_methods(all=true)
    filter_methods(:private_names, all)
  end

  def protected_instance_methods(all=true)
    filter_methods(:protected_names, all)
  end

  def filter_methods(filter, all)
    names = method_table.__send__(filter)
    unless all or self.is_a?(MetaClass) or self.is_a?(IncludedModule)
      return names.map { |name| name.to_s }
    end

    excludes = method_table.map { |name, meth| meth == false ? name : nil }
    undefed = excludes.compact

    sup = direct_superclass

    while sup
      names |= sup.method_table.__send__(filter)

      excludes = method_table.map { |name, meth| meth == false ? name : nil }
      undefed += excludes.compact

      sup = sup.direct_superclass
    end

    (names - undefed).map { |name| name.to_s }
  end
  # private :filter_methods

  def define_method(name, meth = nil, &prc)
    meth ||= prc

    if meth.kind_of?(Proc)
      block_env = meth.block
      cm = DelegatedMethod.new(:call_on_instance, block_env, true)
    elsif meth.kind_of?(Method)
      cm = DelegatedMethod.new(:call, meth, false)
    elsif meth.kind_of?(UnboundMethod)
      cm = DelegatedMethod.new(:call_on_instance, meth, true)
    else
      raise TypeError, "wrong argument type #{meth.class} (expected Proc/Method)"
    end

    self.method_table[name.to_sym] = cm
    Rubinius::VM.reset_method_cache(name.to_sym)
    meth
  end

  def extend_object(obj)
    append_features obj.metaclass
  end

  def include?(mod)
    if !mod.kind_of?(Module) or mod.kind_of?(Class)
      raise TypeError, "wrong argument type #{mod.class} (expected Module)"
    end
    ancestors.include? mod
  end

  def included_modules
    out = []
    sup = direct_superclass

    while sup
      if sup.class == IncludedModule
        out << sup.module
      end

      sup = sup.direct_superclass
    end

    out
  end

  def set_visibility(meth, vis, where = nil)
    name = Type.coerce_to_symbol(meth)
    vis = vis.to_sym

    if entry = method_table[name] then
      if entry.kind_of? Executable then
        entry = CompiledMethod::Visibility.new entry.dup, vis
      else
        entry = entry.dup
        entry.visibility = vis
      end

      method_table[name] = entry
    elsif find_method_in_hierarchy(name) then
      method_table[name] = CompiledMethod::Visibility.new nil, vis
    else
      raise NoMethodError, "Unknown #{where}method '#{name}' to make #{vis.to_s} (#{self})"
    end

    Rubinius::VM.reset_method_cache name

    return name
  end

  def set_class_visibility(meth, vis)
    metaclass.set_visibility meth, vis, "class "
  end

  def protected(*args)
    if args.empty?
      MethodContext.current.sender.method_scope = :protected
      return
    end

    args.each { |meth| set_visibility(meth, :protected) }
  end

  def public(*args)
    if args.empty?
      MethodContext.current.sender.method_scope = nil
      return
    end

    args.each { |meth| set_visibility(meth, :public) }
  end

  def private_class_method(*args)
    args.each do |meth|
      set_class_visibility(meth, :private)
    end
    self
  end

  def public_class_method(*args)
    args.each do |meth|
      set_class_visibility(meth, :public)
    end
    self
  end

  def module_exec(*args, &prc)
    instance_exec(*args, &prc)
  end
  alias_method :class_exec, :module_exec

  def constants
    constants = self.constants_table.keys
    current = self.direct_superclass

    while current != nil && current != Object
      constants += current.constants_table.keys
      current = current.direct_superclass
    end

    constants.map { |c| c.to_s }
  end

  def const_defined?(name)
    name = normalize_const_name(name)

    current = self
    while current
      return true if current.constants_table.has_key?(name)
      current = current.direct_superclass
    end

    return false
  end

  # Check if a full constant path is defined, e.g. SomeModule::Something
  def const_path_defined?(name)
    # Start at Object if this is a fully-qualified path
    if name[0,2] == "::" then
      start = Object
      pieces = name[2,(name.length - 2)].split("::")
    else
      start = self
      pieces = name.split("::")
    end

    defined = false
    current = start
    while current and not defined
      const = current
      defined = pieces.all? do |piece|
        if const.is_a?(Module) and const.constants_table.key?(piece)
          const = const.constants_table[piece].value
          true
        end
      end
      current = current.direct_superclass
    end
    return defined
  end

  def const_set(name, value)
    if value.is_a? Module
      value.set_name_if_necessary(name, self)
    end

    name = normalize_const_name(name)
    assoc = constant_table[name]

    if assoc
      assoc.value = value
    else
      constants_table[name] = LookupTable::Association.new(name, value)
    end

    return value
  end

  ##
  # \_\_const_set__ is emitted by the compiler for const assignment in
  # userland.

  def clear_associations(name)
    return if RecursionGuard.inspecting?(self)

    RecursionGuard.inspect(self) do
      if assoc = @constants[name]
        assoc.active = false
        @constants[name] = LookupTable::Association.new(name, assoc.value)
      end

      @constants.each do |key, assoc|
        if assoc.value.kind_of? Module
          assoc.value.clear_associations(name)
        end
      end

      if ds = direct_superclass() and ds != Object
        ds.clear_associations(name)
      end
    end
  end

  def __const_set__(name, value)
    const_name = normalize_const_name(name)

    # check all subscopes and invalidate Associations found for +const_name+
    @constants.each_entry do |key, assoc|
      if assoc.value.kind_of? Module
        assoc.value.clear_associations(const_name)
      end
    end

    mod = self
    while mod
      assoc = mod.constants_table[const_name]
      break if assoc
      mod = mod.direct_superclass
    end

    if mod
      if mod == self
        warn "already initialized constant #{name}"
      else
        # We're masking an existing constant. Invalid it.
        assoc.active = false
        mod.constants_table[const_name] =
          LookupTable::Association.new(const_name, assoc.value)
      end
    end

    return const_set(name, value)
  end

  ##
  # Return the named constant enclosed in this Module.
  #
  # Included Modules and, for Class objects, superclasses are also searched.
  # Modules will in addition look in Object. The name is attempted to convert
  # using #to_str. If the constant is not found, #const_missing is called
  # with the name.

  def const_get(name)
    recursive_const_get(name)
  end

  def const_lookup(name)
    mod = self

    parts = String(name).split '::'
    parts.each do |part| mod = mod.const_get part end

    mod
  end

  def const_missing(name)
    raise NameError, "Missing or uninitialized constant: #{name}"
  end

  def attr(name,writeable=false)
    attr_reader(name)
    attr_writer(name) if writeable
    return nil
  end

  def <(other)
    unless other.kind_of? Module
      raise TypeError, "compared with non class/module"
    end
    return false if self.equal? other
    ancestors.index(other) && true
  end

  def <=(other)
    return true if self.equal? other
    lt = self < other
    return false if lt.nil? && other < self
    lt
  end

  def >(other)
    unless other.kind_of? Module
      raise TypeError, "compared with non class/module"
    end
    other < self
  end

  def >=(other)
    unless other.kind_of? Module
      raise TypeError, "compared with non class/module"
    end
    return true if self.equal? other
    gt = self > other
    return false if gt.nil? && other > self
    gt
  end

  def <=>(other)
    return 0 if self.equal? other
    return nil unless other.kind_of? Module
    lt = self < other
    if lt.nil?
      other < self ? 1 : nil
    else
      lt ? -1 : 1
    end
  end

  def ===(inst)
    return true if inst.kind_of? self
    # TODO: check if inst is extended by self
    # inst.metaclass < self & true rescue false
    false
  end

  def set_name_if_necessary(name, mod)
    return unless @name.nil?
    parts = [name.to_s]
    while mod and mod != Object
      parts.unshift mod.name
      mod = mod.encloser
    end
    @name = parts.join("::").to_sym
  end

  # Install a new Autoload object into the constants table
  # See kernel/common/autoload.rb
  def autoload(name, path)
    name = normalize_const_name(name)
    raise ArgumentError, "empty file name" if path.empty?
    trigger = Autoload.new(name, self, path)
    constants_table[name] = LookupTable::Association.new(name, trigger)
    return nil
  end

  # Is an autoload trigger defined for the given path?
  def autoload?(name)
    name = name.to_sym
    return unless constants_table.key?(name)
    trigger = constants_table[name].value
    return unless trigger.kind_of?(Autoload)
    trigger.original_path
  end

  def remove_const(name)
    sym = name.to_sym
    unless constants_table.has_key?(sym)
      return const_missing(name)
    end

    assoc = constants_table.delete(sym)

    val = assoc.value

    # Silly API compac. Shield Autoload instances
    return nil if val.kind_of? Autoload
    val
  end

  private :remove_const

  def extended(name)
  end

  private :extended

  def method_added(name)
  end

  private :method_added

  # See #const_get for documentation.
  def recursive_const_get(name, missing=true)
    name = normalize_const_name(name)

    current, constant = self, Undefined

    while current
      assoc = current.constants_table.fetch name, Undefined
      unless assoc.equal?(Undefined)
        constant = assoc.value
        constant = constant.call if constant.kind_of?(Autoload)
        return constant
      end

      current = current.direct_superclass
    end

    if instance_of?(Module)
      assoc = Object.constants_table.fetch name, Undefined
      unless assoc.equal?(Undefined)
        constant = assoc.value
        constant = constant.call if constant.kind_of?(Autoload)
        return constant
      end
    end

    return nil unless missing

    const_missing(name)
  end

  private :recursive_const_get

  def normalize_const_name(name)
    name = Type.coerce_to_symbol(name)
    raise NameError, "wrong constant name #{name}" unless valid_const_name?(name)
    name
  end

  private :normalize_const_name

  #--
  # Modified to fit definition at:
  # http://docs.huihoo.com/ruby/ruby-man-1.4/syntax.html#variable
  #++

  def valid_const_name?(name)
    name.to_s =~ /^((::)?[A-Z]\w*)+$/ ? true : false
  end

  private :valid_const_name?

  def initialize_copy(other)
    @method_table = @method_table.dup

    old_constants = @constants
    new_constants = LookupTable.new

    old_constants.each do |name, assoc|
      new_assoc = assoc.dup
      new_constants[name] = new_assoc

      val = new_assoc.value
      if val.kind_of? Autoload
        new_assoc.value = Autoload.new(val.name, self, val.original_path)
      end
    end

    @constants = new_constants
  end

end
