#include <cstdlib>
#include <iostream>

#include "objectmemory.hpp"
#include "gc_marksweep.hpp"

namespace rubinius {


  /* ObjectMemory methods */
  ObjectMemory::ObjectMemory(size_t young_bytes)
               :young(this, young_bytes), mature(this) {

    remember_set = new ObjectArray(0);

    collect_young_now = false;
    collect_mature_now = false;
    large_object_threshold = 2700;
    young.lifetime = 6;
    last_object_id = 0;

    for(size_t i = 0; i < LastObjectType; i++) {
      type_info[i] = NULL;
    }
  }

  ObjectMemory::~ObjectMemory() {

    young.free_objects();
    mature.free_objects();

    delete remember_set;

    for(size_t i = 0; i < LastObjectType; i++) {
      if(type_info[i]) delete type_info[i];
    }
  }

  void ObjectMemory::set_young_lifetime(size_t age) {
    young.lifetime = age;
  }

  void ObjectMemory::debug_marksweep(bool val) {
    if(val) {
      mature.free_entries = false;
    } else {
      mature.free_entries = true;
    }
  }

  bool ObjectMemory::valid_object_p(OBJECT obj) {
    if(obj->young_object_p()) {
      return young.current->contains_p(obj);
    } else if(obj->mature_object_p()) {
      return true;
    } else {
      return false;
    }
  }

  /* Garbage collection */

  OBJECT ObjectMemory::promote_object(OBJECT obj) {
    OBJECT copy = mature.copy_object(obj);
    copy->zone = MatureObjectZone;
    return copy;
  }

  void ObjectMemory::collect_young(ObjectArray &roots) {
    young.collect(roots);
  }

  void ObjectMemory::collect_mature(ObjectArray &roots) {
    mature.collect(roots);
    young.clear_marks();
  }

  TypeInfo* ObjectMemory::get_type_info(Class* cls) {
    TypeInfo *ti = new TypeInfo(cls);
    type_info[cls->object_type->n2i()] = ti;
    return ti;
  }

  TypeInfo* ObjectMemory::find_type_info(OBJECT obj) {
    return type_info[obj->obj_type];
  }

  TypeInfo::TypeInfo(Class *cls) {
    type = (object_type)cls->object_type->n2i();
    cleanup = NULL;
    state = NULL;
  }

  void TypeInfo::delete_object(OBJECT obj) {
    if(cleanup) cleanup(state, obj);
  }

};

void* XMALLOC(size_t bytes) {
  return malloc(bytes);
}

void XFREE(void* ptr) {
  free(ptr);
}

void* XREALLOC(void* ptr, size_t bytes) {
  return realloc(ptr, bytes);
}

void* XCALLOC(size_t items, size_t bytes) {
  return calloc(items, bytes);
}
