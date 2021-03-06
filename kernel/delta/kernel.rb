module Kernel

  ##
  #--
  # HACK todo handle cascading raises (ie, TypeError raise
  # raising forever blows)
  #++

  def raise(exc=Undefined, msg=nil, trace=nil)
    skip = false
    if exc.equal? Undefined
      exc = $!
      if exc
        skip = true
      else
        exc = RuntimeError.new("No current exception")
      end
    elsif exc.respond_to? :exception
      exc = exc.exception msg
      raise ::TypeError, 'exception class/object expected' unless exc.kind_of?(::Exception)
      exc.set_backtrace trace if trace
    elsif exc.kind_of? String or !exc
      exc = ::RuntimeError.exception exc
    else
      raise ::TypeError, 'exception class/object expected'
    end

    if $DEBUG and $VERBOSE != nil
      sender = MethodContext.current.sender
      STDERR.puts "Exception: `#{exc.class}' #{sender.location} - #{exc.message}"
    end

    if !skip and !exc.context
      exc.context = MethodContext.current.sender
    end

    Rubinius.asm(exc) { |e| e.bytecode(self); raise_exc }
  end
  module_function :raise

  alias_method :fail, :raise
  module_function :fail

  def method_missing(meth, *args)
    # Exclude method_missing from the backtrace since it only confuses
    # people.
    myself = MethodContext.current
    ctx = myself.sender

    if myself.send_private?
      Kernel.raise NameError, "undefined local variable or method `#{meth}' for #{inspect}"
    elsif self.__kind_of__ Class or self.__kind_of__ Module
      Kernel.raise NoMethodError.new("No method '#{meth}' on #{self} (#{self.__class__})", ctx, args)
    else
      Kernel.raise NoMethodError.new("No method '#{meth}' on an instance of #{self.__class__}.", ctx, args)
    end
  end

  private :method_missing

  # Add in $! in as a hook, to just do $!. This is for accesses to $!
  # that the compiler can't see.
  get = proc { $! }
  Globals.set_hook(:$!, get, nil)

  # Same as $!, for any accesses we might miss.
  # HACK. I doubt this is correct, because of how it will be called.
  get = proc { Regex.last_match }
  Globals.set_hook(:$~, get, nil)

  get = proc { ARGV }
  Globals.set_hook(:$*, get, nil)

  get = proc { $! ? $!.backtrace : nil }
  Globals.set_hook(:$@, get, nil)

  get = proc { Process.pid }
  Globals.set_hook(:$$, get, nil)
end
