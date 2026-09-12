#include "types.h"

#include <algorithm>

namespace {
const int64_t kWord = 8;
}

TypeTable::TypeTable() {
  Type t;

  t = Type{}; t.kind = TY_VOID;
  void_ty = add(t);
  by_name["void"] = void_ty;

  t = Type{}; t.kind = TY_BOOL; t.bits = 1;
  bool_ty = add(t);
  by_name["bool"] = bool_ty;

  struct { const char *name; int bits; bool is_signed; } ints[] = {
      {"i8", 8, true},    {"i16", 16, true},  {"i32", 32, true},
      {"i64", 64, true},  {"int", 64, true},  {"u8", 8, false},
      {"u16", 16, false}, {"u32", 32, false}, {"u64", 64, false},
      {"uint", 64, false},
  };
  for (auto &spec : ints) {
    t = Type{};
    t.kind = TY_INT;
    t.bits = spec.bits;
    t.is_signed = spec.is_signed;
    by_name[spec.name] = add(t);
  }
  int_ty = by_name["int"];
  u8_ty = by_name["u8"];
  usize_ty = by_name["uint"];

  for (int bits : {32, 64}) {
    t = Type{};
    t.kind = TY_FLOAT;
    t.bits = bits;
    by_name[bits == 32 ? "f32" : "f64"] = add(t);
  }

  t = Type{};
  t.kind = TY_STRING;
  t.elem = u8_ty;
  string_ty = add(t);
  by_name["string"] = string_ty;

  t = Type{};
  t.kind = TY_INT;
  t.bits = 16;
  t.name = "error";
  error_ty = add(t);
  by_name["error"] = error_ty;

  t = Type{};
  t.kind = TY_STRUCT;
  t.is_any = true;
  t.name = "any";
  any_ty = add(t);
  by_name["any"] = any_ty;
  {
    std::vector<Field> fields;
    // Named as an API rather than as internals: reading an `any` is just
    // reading these four fields.
    fields.push_back(Field{"Kind", by_name["u8"], 0, 0});
    fields.push_back(Field{"Int", by_name["i64"], 0, 0});
    fields.push_back(Field{"Real", by_name["f64"], 0, 0});
    fields.push_back(Field{"Text", string_ty, 0, 0});
    layout_struct(any_ty, std::move(fields));
  }

  t = Type{};
  t.kind = TY_OPT;
  t.untyped = true;
  t.elem = void_ty;
  untyped_nil = add(t);

  t = Type{};
  t.kind = TY_INT;
  t.bits = 64;
  t.is_signed = true;
  t.untyped = true;
  untyped_int = add(t);

  t = Type{};
  t.kind = TY_FLOAT;
  t.bits = 64;
  t.untyped = true;
  untyped_float = add(t);
}

Type *TypeTable::error_union(Type *value) {
  auto found = error_unions.find(value);
  if (found != error_unions.end()) return found->second;

  Type t;
  t.kind = TY_STRUCT;
  t.is_error_union = true;
  t.elem = value;
  t.name = "err." + std::to_string(error_unions.size());
  Type *type = add(t);

  std::vector<Field> fields;
  fields.push_back(Field{"code", error_ty, 0, 0});
  if (value->kind != TY_VOID) fields.push_back(Field{"value", value, 0, 0});
  layout_struct(type, std::move(fields));

  error_union_list.push_back(type);
  return error_unions[value] = type;
}

int TypeTable::error_code(const std::string &name) {
  for (size_t i = 0; i < error_list.size(); i++)
    if (error_list[i] == name) return (int)i + 1;
  error_list.push_back(name);
  return (int)error_list.size();
}

Type *TypeTable::add(Type t) {
  pool.push_back(std::move(t));
  return &pool.back();
}

Type *TypeTable::named(const std::string &name) {
  auto found = by_name.find(name);
  return found == by_name.end() ? nullptr : found->second;
}

Type *TypeTable::ptr(Type *elem) {
  auto found = ptrs.find(elem);
  if (found != ptrs.end()) return found->second;
  Type t;
  t.kind = TY_PTR;
  t.elem = elem;
  return ptrs[elem] = add(t);
}

Type *TypeTable::rawptr(Type *elem) {
  auto found = rawptrs.find(elem);
  if (found != rawptrs.end()) return found->second;
  Type t;
  t.kind = TY_RAWPTR;
  t.elem = elem;
  return rawptrs[elem] = add(t);
}

Type *TypeTable::slice(Type *elem) {
  auto found = slices.find(elem);
  if (found != slices.end()) return found->second;
  Type t;
  t.kind = TY_SLICE;
  t.elem = elem;
  return slices[elem] = add(t);
}

Type *TypeTable::array(Type *elem, int64_t count) {
  auto key = std::make_pair(elem, count);
  auto found = arrays.find(key);
  if (found != arrays.end()) return found->second;
  Type t;
  t.kind = TY_ARRAY;
  t.elem = elem;
  t.count = count;
  return arrays[key] = add(t);
}

Type *TypeTable::opt(Type *elem) {
  auto found = opts.find(elem);
  if (found != opts.end()) return found->second;

  // A pointer already has a spare bit pattern, so `?*T` is just the pointer
  // with null standing for absence. Anything else needs a flag beside it.
  if (elem->kind == TY_PTR || elem->kind == TY_RAWPTR) {
    Type t;
    t.kind = TY_OPT;
    t.elem = elem;
    return opts[elem] = add(t);
  }

  Type t;
  t.kind = TY_STRUCT;
  t.is_optional = true;
  t.elem = elem;
  t.name = "opt." + std::to_string(optional_list.size());
  Type *type = add(t);

  std::vector<Field> fields;
  fields.push_back(Field{"has", bool_ty, 0, 0});
  fields.push_back(Field{"value", elem, 0, 0});
  layout_struct(type, std::move(fields));

  optional_list.push_back(type);
  return opts[elem] = type;
}

Type *TypeTable::atomic(Type *elem) {
  auto found = atomics.find(elem);
  if (found != atomics.end()) return found->second;
  Type t;
  t.kind = TY_ATOMIC;
  t.elem = elem;
  return atomics[elem] = add(t);
}

// A mutex beside the value it protects. All zeroes is an unlocked guard, so a
// fresh shared needs no constructor — which is what lets one sit in an array or
// a struct field.
Type *TypeTable::shared(Type *elem) {
  auto found = shareds.find(elem);
  if (found != shareds.end()) return found->second;

  Type t;
  t.kind = TY_STRUCT;
  t.is_shared = true;
  t.elem = elem;
  t.name = "shared." + std::to_string(shared_list.size());
  Type *type = add(t);

  std::vector<Field> fields;
  fields.push_back(
      Field{"guard", array(usize_ty, SWORD_GUARD_SIZE / 8), 0, 0});
  fields.push_back(Field{"value", elem, 0, 0});
  layout_struct(type, std::move(fields));

  shared_list.push_back(type);
  return shareds[elem] = type;
}

Type *TypeTable::func(std::vector<Type *> params, Type *ret) {
  Type t;
  t.kind = TY_FUNC;
  t.params = std::move(params);
  t.ret = ret;
  return add(t);
}

Type *TypeTable::declare_interface(const std::string &name) {
  Type t;
  t.kind = TY_STRUCT;
  t.is_interface = true;
  t.name = name;
  Type *type = add(t);
  by_name[name] = type;

  Type *opaque = rawptr(u8_ty);
  std::vector<Field> fields;
  fields.push_back(Field{"data", opaque, 0, 0});
  fields.push_back(Field{"vtable", opaque, 0, 0});
  layout_struct(type, std::move(fields));
  return type;
}

int TypeTable::vtable(const std::string &key, std::vector<std::string> entries) {
  for (size_t i = 0; i < vtable_list.size(); i++)
    if (vtable_list[i].key == key) return (int)i;
  vtable_list.push_back(VTable{key, std::move(entries)});
  return (int)vtable_list.size() - 1;
}

Type *TypeTable::declare_struct(const std::string &name) {
  Type t;
  t.kind = TY_STRUCT;
  t.name = name;
  return by_name[name] = add(t);
}

void TypeTable::layout_struct(Type *type, std::vector<Field> fields) {
  if (!type->is_extern) {
    // Stable sort by descending alignment: every field then sits at a natural
    // offset and padding only ever appears once, at the end.
    std::stable_sort(fields.begin(), fields.end(),
                     [](const Field &a, const Field &b) {
                       return align_of(a.type) > align_of(b.type);
                     });
  }

  int64_t offset = 0;
  for (size_t i = 0; i < fields.size(); i++) {
    int64_t align = align_of(fields[i].type);
    offset = (offset + align - 1) / align * align;
    fields[i].offset = offset;
    fields[i].index = (int)i;
    offset += size_of(fields[i].type);
  }
  type->fields = std::move(fields);
}

std::string type_str(const Type *t) {
  if (!t) return "<none>";
  switch (t->kind) {
  case TY_VOID: return "void";
  case TY_BOOL: return "bool";
  case TY_INT: {
    if (t->untyped) return "untyped int";
    if (!t->name.empty()) return t->name;
    return (t->is_signed ? "i" : "u") + std::to_string(t->bits);
  }
  case TY_FLOAT:
    return t->untyped ? "untyped float" : "f" + std::to_string(t->bits);
  case TY_PTR: return "*" + type_str(t->elem);
  case TY_RAWPTR: return "[*]" + type_str(t->elem);
  case TY_SLICE: return "[]" + type_str(t->elem);
  case TY_STRING: return "string";
  case TY_ARRAY:
    return "[" + std::to_string(t->count) + "]" + type_str(t->elem);
  case TY_OPT: return t->untyped ? "nil" : "?" + type_str(t->elem);
  case TY_ATOMIC: return "atomic[" + type_str(t->elem) + "]";
  case TY_STRUCT:
    if (t->is_error_union) return "!" + type_str(t->elem);
    if (t->is_optional) return "?" + type_str(t->elem);
    if (t->is_shared) return "shared[" + type_str(t->elem) + "]";
    return t->name;
  case TY_FUNC: {
    std::string s = "func(";
    for (size_t i = 0; i < t->params.size(); i++) {
      if (i) s += ", ";
      s += type_str(t->params[i]);
    }
    s += ")";
    if (t->ret && t->ret->kind != TY_VOID) s += " " + type_str(t->ret);
    return s;
  }
  }
  return "?";
}

bool type_eq(const Type *a, const Type *b) {
  if (a == b) return true;
  if (!a || !b || a->kind != b->kind) return false;
  switch (a->kind) {
  case TY_INT:
    // The name keeps `error` distinct from the u16 it is represented by.
    return a->bits == b->bits && a->is_signed == b->is_signed &&
           a->name == b->name;
  case TY_FLOAT: return a->bits == b->bits;
  case TY_PTR: case TY_RAWPTR: case TY_SLICE: case TY_OPT: case TY_ATOMIC:
    return type_eq(a->elem, b->elem);
  case TY_ARRAY:
    return a->count == b->count && type_eq(a->elem, b->elem);
  case TY_STRUCT: return a->name == b->name;
  default: return true;
  }
}

bool assignable(const Type *from, const Type *to) {
  if (type_eq(from, to)) return true;
  if (!from || !to) return false;
  // `nil` fits any optional, and a present value fits the optional over it.
  if (is_optional(to)) {
    if (from->kind == TY_OPT && from->untyped) return true;
    return assignable(from, opt_payload(to));
  }
  // An untyped integer fits a float, but not the other way round: that would
  // silently drop a fraction.
  if (from->untyped && from->kind == TY_INT)
    return to->kind == TY_INT || to->kind == TY_FLOAT;
  if (from->untyped && from->kind == TY_FLOAT) return to->kind == TY_FLOAT;
  // A string is a []u8 that promises not to change, so it converts one way.
  if (from->kind == TY_STRING && to->kind == TY_SLICE && to->elem->bits == 8)
    return true;
  return false;
}

bool is_numeric(const Type *t) {
  return t && (t->kind == TY_INT || t->kind == TY_FLOAT);
}

bool is_integer(const Type *t) { return t && t->kind == TY_INT; }

bool is_float(const Type *t) { return t && t->kind == TY_FLOAT; }

bool is_atomic(const Type *t) { return t && t->kind == TY_ATOMIC; }

bool is_shared(const Type *t) { return t && t->is_shared; }

int any_kind_of(const Type *t) {
  if (!t) return -1;
  switch (t->kind) {
  case TY_BOOL: return ANY_BOOL;
  case TY_INT: return t->is_signed ? ANY_INT : ANY_UINT;
  case TY_FLOAT: return ANY_FLOAT;
  case TY_STRING: return ANY_STRING;
  case TY_PTR: case TY_RAWPTR: return ANY_POINTER;
  default: return -1;
  }
}

bool is_optional(const Type *t) {
  return t && !t->untyped && (t->kind == TY_OPT || t->is_optional);
}

const Type *opt_payload(const Type *t) { return t ? t->elem : nullptr; }

bool is_aggregate(const Type *t) {
  if (!t) return false;
  switch (t->kind) {
  case TY_SLICE: case TY_STRING: case TY_ARRAY: case TY_STRUCT:
    return true;
  default:
    return false;
  }
}

int64_t size_of(const Type *t) {
  if (!t) return 0;
  switch (t->kind) {
  case TY_VOID: return 0;
  case TY_BOOL: return 1;
  case TY_INT: case TY_FLOAT: return t->bits / 8;
  case TY_PTR: case TY_RAWPTR: case TY_FUNC: return kWord;
  case TY_OPT: return size_of(t->elem); // ?*T reuses the null pointer as tag
  case TY_ATOMIC: return size_of(t->elem);
  case TY_SLICE: case TY_STRING: return 2 * kWord;
  case TY_ARRAY: return t->count * size_of(t->elem);
  case TY_STRUCT: {
    if (t->fields.empty()) return 0;
    const Field &last = t->fields.back();
    int64_t align = align_of(t);
    int64_t size = last.offset + size_of(last.type);
    return (size + align - 1) / align * align;
  }
  }
  return 0;
}

int64_t align_of(const Type *t) {
  if (!t) return 1;
  switch (t->kind) {
  case TY_VOID: return 1;
  case TY_ARRAY: return align_of(t->elem);
  case TY_STRUCT: {
    int64_t align = 1;
    for (const Field &f : t->fields) align = std::max(align, align_of(f.type));
    return align;
  }
  default: {
    int64_t size = size_of(t);
    return size > kWord ? kWord : (size ? size : 1);
  }
  }
}

const Field *find_field(const Type *t, const std::string &name) {
  if (!t || t->kind != TY_STRUCT) return nullptr;
  for (const Field &f : t->fields)
    if (f.name == name) return &f;
  return nullptr;
}
