#ifndef RBX_BUILTIN_BLOCK_WRAPPER_HPP
#define RBX_BUILTIN_BLOCK_WRAPPER_HPP

#include "builtin/object.hpp"
#include "type_info.hpp"

namespace rubinius {
  class BlockEnvironment;

  class BlockWrapper : public Object {
  public:
    const static object_type type = BlockWrapperType;

  private:
    BlockEnvironment* block_; // slot

  public:
    attr_accessor(block, BlockEnvironment);

    static void init(STATE);

    // Ruby.primitive :block_wrapper_allocate
    static BlockWrapper* create(STATE, Object* self);

    void call(STATE, Task* task, size_t args);

    // Ruby.primitive? :block_wrapper_call
    ExecuteStatus call_prim(STATE, Executable* exec, Task* task, Message& msg);

    // Ruby.primitive :block_wrapper_from_env
    static BlockWrapper* from_env(STATE, BlockEnvironment* env);

    class Info : public TypeInfo {
    public:
      BASIC_TYPEINFO(TypeInfo)
    };
  };
}

#endif
