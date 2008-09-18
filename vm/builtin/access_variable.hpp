#ifndef RBX_BUILTIN_ACCESS_VARIABLE_HPP
#define RBX_BUILTIN_ACCESS_VARIABLE_HPP

#include "builtin/executable.hpp"
#include "vm.hpp"

namespace rubinius {

  class InstructionSequence;
  class MemoryPointer;
  class VMMethod;
  class StaticScope;

  class AccessVariable : public Executable {
  public:
    const static size_t fields = Executable::fields + 2;
    const static object_type type = AccessVariableType;

  private:
    SYMBOL name_;  // slot
    OBJECT write_; // slot

  public:
    /* accessors */

    attr_accessor(name, Symbol);
    attr_accessor(write, Object);

    /* interface */

    static void init(STATE);
    // Ruby.primitive :accessvariable_allocate
    static AccessVariable* allocate(STATE);
    static bool access_execute(STATE, Executable* meth, Task* task, Message& msg);

    class Info : public TypeInfo {
    public:
      BASIC_TYPEINFO(TypeInfo)
    };
  };
}

#endif