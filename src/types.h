#pragma once

#include <cstdint>
#include <deque>
#include <map>
#include <string>
#include <unordered_map>
#include <vector>

enum TypeKind {
  TY_VOID,
  TY_BOOL,
  TY_INT,
  TY_FLOAT,
  TY_PTR,    // *T, never null
  TY_RAWPTR, // [*]T, no length, FFI only
  TY_SLICE,  // []T, pointer + length
  TY_STRING, // immutable []u8 with its own identity
  TY_ARRAY,  // [N]T, by value
  TY_OPT,    // ?T
  TY_ATOMIC, // atomic[T]: T's representation, reached only through its
             // own operations, which is what lets it cross between tasks
  TY_STRUCT,
  TY_FUNC,
};

struct Type;

struct Field {
  std::string name;
  Type *type = nullptr;
  int64_t offset = 0;
  int index = 0; // position after layout, which is what the backend indexes by
};

struct Type {
  TypeKind kind = TY_VOID;
  int bits = 0;
  bool is_signed = false;
  bool untyped = false;
  bool is_extern = false; // struct laid out in declaration order, for C
  // `!T` is a struct of { code, value } underneath, which gives it layout,
  // field access and codegen for free. `?T` does the same for payloads that
  // have no spare bit pattern; over a pointer it is just the pointer.
  bool is_error_union = false;
  bool is_optional = false;
  // An interface is a { data, vtable } pair: a struct for layout purposes,
  // with the method signatures kept alongside for checking.
  bool is_interface = false;
  // `any`: what a variadic argument is boxed into. It carries the value and
  // enough of a tag to say what the value is.
  bool is_any = false;
  Type *elem = nullptr;
  int64_t count = 0; // TY_ARRAY length
  std::string name;  // TY_STRUCT
  std::vector<Field> fields;
  std::vector<Field> methods; // TY_STRUCT with is_interface
  std::vector<Type *> params;
  Type *ret = nullptr;
};

struct TypeTable {
  TypeTable();

  Type *named(const std::string &name);
  Type *ptr(Type *elem);
  Type *rawptr(Type *elem);
  Type *slice(Type *elem);
  Type *array(Type *elem, int64_t count);
  Type *opt(Type *elem);
  Type *atomic(Type *elem);
  Type *func(std::vector<Type *> params, Type *ret);

  Type *declare_struct(const std::string &name);
  Type *declare_interface(const std::string &name);
  // Assigns offsets. Non-extern structs are reordered widest-first so padding
  // does not leak into every instance.
  void layout_struct(Type *type, std::vector<Field> fields);

  Type *error_union(Type *value);
  // Every `!T` the program mentions, in creation order, so the backend can
  // emit a named type for each.
  const std::vector<Type *> &error_unions_made() const {
    return error_union_list;
  }
  // Optionals over a non-pointer payload are structs too, and need naming.
  const std::vector<Type *> &optionals_made() const { return optional_list; }
  // Instantiated generic structs, which the backend has to name as well.
  void note_instance(Type *type) { instance_list.push_back(type); }
  const std::vector<Type *> &instances_made() const { return instance_list; }

  // Error names are global to the program, as in Zig: every name gets one code
  // and code 0 always means success. No interprocedural inference needed.
  int error_code(const std::string &name);
  const std::vector<std::string> &errors() const { return error_list; }

  // One vtable per (concrete type, interface) pair actually used.
  struct VTable {
    std::string key;
    std::vector<std::string> entries; // mangled Type.method names, in order
  };
  int vtable(const std::string &key, std::vector<std::string> entries);
  const std::vector<VTable> &vtables() const { return vtable_list; }

  Type *void_ty;
  Type *bool_ty;
  Type *int_ty;
  Type *u8_ty;
  Type *usize_ty;
  Type *string_ty;
  Type *error_ty;
  Type *any_ty;
  Type *untyped_nil;
  Type *untyped_int;
  Type *untyped_float;

private:
  Type *add(Type t);
  std::deque<Type> pool;
  std::unordered_map<std::string, Type *> by_name;
  std::unordered_map<Type *, Type *> ptrs, rawptrs, slices, opts, error_unions;
  std::unordered_map<Type *, Type *> atomics;
  std::map<std::pair<Type *, int64_t>, Type *> arrays;
  std::vector<std::string> error_list;
  std::vector<Type *> error_union_list;
  std::vector<Type *> optional_list;
  std::vector<Type *> instance_list;
  std::vector<VTable> vtable_list;
};

std::string type_str(const Type *t);
bool type_eq(const Type *a, const Type *b);
bool assignable(const Type *from, const Type *to);

bool is_numeric(const Type *t);
bool is_integer(const Type *t);
bool is_float(const Type *t);
bool is_atomic(const Type *t);

// The tags an `any` can carry. std/fmt mirrors these; they are part of the
// language rather than of that package.
enum AnyKind {
  ANY_NONE = 0, ANY_BOOL = 1, ANY_INT = 2, ANY_UINT = 3,
  ANY_FLOAT = 4, ANY_STRING = 5, ANY_POINTER = 6,
};
int any_kind_of(const Type *t); // -1 when the type cannot be boxed

// `?T`: either a pointer using null as its tag, or a { has, value } pair.
bool is_optional(const Type *t);
const Type *opt_payload(const Type *t);

// Aggregates live in memory: an expression of aggregate type evaluates to its
// address, and assigning one copies bytes.
bool is_aggregate(const Type *t);

int64_t size_of(const Type *t);
int64_t align_of(const Type *t);

const Field *find_field(const Type *t, const std::string &name);
