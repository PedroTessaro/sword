#include "check.h"

#include "package.h"

#include <cctype>
#include <functional>
#include <set>

#include <unordered_map>
#include <unordered_set>

namespace {

struct Checker {
  Program &prog;
  Package &pkg;
  Ast &ast;
  TypeTable &types;

  std::vector<std::unordered_map<std::string, Symbol *>> scopes;
  // Type parameters in scope while an instantiation is being checked.
  std::vector<std::pair<std::string, Type *>> bindings;
  Type *ret_type = nullptr;
  int loop_depth = 0;
  std::vector<Node *> open_scopes;
  std::vector<int> scope_loop_base;

  // What a scope's tasks reach into, and how. Two accesses to the same memory
  // are a race unless both only read, or they are pieces of one partition.
  struct Access {
    Symbol *root = nullptr;  // the binding the memory hangs off
    Symbol *chunk = nullptr; // set when the value is a piece of a partition
    bool writes = false;
    // A spawn inside a loop happens once per iteration, so its accesses have
    // to be compared against themselves.
    bool repeated = false;
    Pos at;
  };
  std::vector<std::vector<Access>> scope_uses;

  // Tasks borrow the frame the scope sits in, so control may not leave the
  // block while they are still running. The join is the closing brace.
  bool leaving_scope(Node *n, const char *what) {
    if (open_scopes.empty()) return false;
    const char *kind =
        open_scopes.back()->kind == ND_SCOPE ? "scope" : "parallel for";
    error(n->pos,
          "cannot %s out of a '%s': its tasks are still using this frame, and "
          "the join is the closing brace",
          what, kind);
    return true;
  }

  Checker(Program &p, Package &k, TypeTable &t)
      : prog(p), pkg(k), ast(p.ast), types(t) {}

  // --- packages ----------------------------------------------------------

  Package *imported(const std::string &name) {
    if (name.empty()) return nullptr;
    for (const std::string &path : pkg.imports) {
      Package *other = prog.by_path[path];
      if (other && other->name == name) return other;
    }
    return nullptr;
  }

  // A name is visible outside its package only when it starts uppercase.
  template <class Map>
  auto find_in(Map &table, const std::string &name, bool same_package)
      -> decltype(table.begin()->second) {
    auto found = table.find(name);
    if (found == table.end()) return nullptr;
    if (!same_package && !exported(name)) return nullptr;
    return found->second;
  }

  // Plain name: a bound type parameter wins, then the package's own types,
  // then the builtins.
  Type *lookup_type(const std::string &name) {
    for (auto it = bindings.rbegin(); it != bindings.rend(); ++it)
      if (it->first == name) return it->second;
    auto found = pkg.type_names.find(name);
    if (found != pkg.type_names.end()) return found->second;
    return types.named(name);
  }

  Type *lookup_type(Node *n) {
    if (!n->text.empty()) {
      Package *other = imported(n->text);
      if (!other) {
        error(n->pos, "'%s' is not an imported package", n->text.c_str());
        return nullptr;
      }
      Type *t = find_in(other->type_names, n->name, false);
      if (!t)
        error(n->pos, "package '%s' has no exported type '%s'",
              other->name.c_str(), n->name.c_str());
      return t;
    }
    return lookup_type(n->name);
  }

  void push_scope() { scopes.emplace_back(); }
  void pop_scope() { scopes.pop_back(); }

  Symbol *declare(const std::string &name, Type *type, bool is_mut, Pos pos) {
    auto &scope = scopes.back();
    auto found = scope.find(name);
    if (found != scope.end()) {
      error(pos, "'%s' is already declared in this scope", name.c_str());
      note(found->second->pos, "previous declaration is here");
      return found->second;
    }
    return scope[name] = ast.make_symbol(name, type, is_mut, pos);
  }

  Symbol *lookup(const std::string &name) {
    for (auto it = scopes.rbegin(); it != scopes.rend(); ++it) {
      auto found = it->find(name);
      if (found != it->end()) return found->second;
    }
    return nullptr;
  }

  // --- types -------------------------------------------------------------

  Type *resolve(Node *n) {
    if (!n) return types.void_ty;
    switch (n->kind) {
    case ND_TYPE_NAME: {
      Type *t = lookup_type(n);
      if (!t && n->text.empty())
        error(n->pos, "unknown type '%s'", n->name.c_str());
      return t ? t : types.void_ty;
    }
    case ND_TYPE_PTR: return types.ptr(resolve(n->lhs));
    case ND_TYPE_RAWPTR: return types.rawptr(resolve(n->lhs));
    case ND_TYPE_SLICE: return types.slice(resolve(n->lhs));
    case ND_TYPE_OPT: return types.opt(resolve(n->lhs));
    case ND_TYPE_ARRAY:
      return types.array(resolve(n->lhs), (int64_t)n->ival);
    case ND_TYPE_ERR: {
      Type *value = resolve(n->lhs);
      if (value->is_error_union) {
        error(n->pos, "'!' cannot be applied twice");
        return value;
      }
      return types.error_union(value);
    }
    default:
      error(n->pos, "expected a type");
      return types.void_ty;
    }
  }

  // An untyped literal takes the type of its context. Walking the expression
  // lets `x i64 = 1 + 2` give both literals the right width before lowering.
  void apply_type(Node *n, Type *t) {
    if (!n || !n->type || !n->type->untyped) return;
    n->type = t;
    apply_type(n->lhs, t);
    apply_type(n->rhs, t);
  }

  Type *settle(Node *n) {
    if (n && n->type && n->type->untyped && n->type->kind == TY_INT)
      apply_type(n, types.int_ty);
    return n ? n->type : types.void_ty;
  }

  // --- places ------------------------------------------------------------

  bool is_place(Node *n) {
    switch (n->kind) {
    case ND_IDENT: return n->sym && !n->sym->is_func;
    case ND_FIELD: return n->lhs->type->kind != TY_ARRAY || n->name != "len";
    case ND_INDEX: return true;
    case ND_UNARY: return n->op == TK_STAR;
    default: return false;
    }
  }

  // Mutability belongs to the binding a place is reached through: if the root
  // was not declared `mut`, nothing reachable from it can be written.
  Symbol *place_root(Node *n) {
    switch (n->kind) {
    case ND_IDENT: return n->sym;
    case ND_FIELD: case ND_INDEX: case ND_SLICE_EXPR: return place_root(n->lhs);
    case ND_UNARY: return n->op == TK_STAR ? place_root(n->lhs) : nullptr;
    default: return nullptr;
    }
  }

  bool require_mutable(Node *n, const char *what) {
    Symbol *root = place_root(n);
    if (!root || root->is_mut) return true;
    if (n->kind == ND_IDENT)
      error(n->pos, "cannot %s immutable binding '%s'", what,
            root->name.c_str());
    else
      error(n->pos, "cannot %s data reached through immutable binding '%s'",
            what, root->name.c_str());
    note(root->pos, "declare it with 'mut' to allow mutation");
    return false;
  }

  // --- sharing ------------------------------------------------------------

  // Copying a value is only safe when the copy owns everything it reaches. A
  // slice header copies, but its elements stay behind.
  bool shares_memory(const Type *t, int depth = 0) {
    if (!t || depth > 8) return false;
    switch (t->kind) {
    case TY_SLICE: case TY_PTR: case TY_RAWPTR:
      return true;
    case TY_STRING:
      return false; // immutable by definition
    case TY_ARRAY: case TY_OPT:
      return shares_memory(t->elem, depth + 1);
    case TY_STRUCT:
      if (t->is_interface) return true;
      for (const Field &f : t->fields)
        if (shares_memory(f.type, depth + 1)) return true;
      return false;
    default:
      return false;
    }
  }

  // Where shared memory comes from. Unlike place_root this also follows `&`
  // and pointer conversions: `&counter` reaches the same memory as `counter`.
  Symbol *share_root(Node *n) {
    if (!n) return nullptr;
    switch (n->kind) {
    case ND_IDENT:
      return n->sym;
    case ND_FIELD: case ND_INDEX: case ND_SLICE_EXPR:
      return share_root(n->lhs);
    case ND_UNARY:
      return n->op == TK_STAR || n->op == TK_AMP ? share_root(n->lhs)
                                                 : nullptr;
    case ND_CONVERT:
      return n->kids.empty() ? nullptr : share_root(n->kids[0]);
    default:
      return nullptr;
    }
  }

  Access access_of(Node *value, bool writes) {
    Access use;
    use.writes = writes;
    use.at = value->pos;
    Symbol *root = share_root(value);
    if (!root) return use;
    if (root->chunk_of) {
      use.chunk = root;
      use.root = root->chunk_of;
    } else {
      use.root = root;
    }
    return use;
  }

  static bool clashes(const Access &a, const Access &b) {
    if (!a.root || a.root != b.root) return false;
    if (!a.writes && !b.writes) return false;
    // Two pieces of the same partition never overlap.
    if (a.chunk && a.chunk == b.chunk) return false;
    return true;
  }

  void record_use(const Access &use) {
    if (!use.root) return; // memory with no name behind it: nothing to compare

    if (use.repeated && clashes(use, use)) {
      error(use.at,
            "every turn of this loop hands the same memory to a new task, so "
            "the tasks would overlap in '%s'",
            use.root->name.c_str());
      note(use.root->pos,
           "walk it with 'for part in %s.chunks(n)' to give each task a piece "
           "of its own",
           use.root->name.c_str());
      return;
    }

    for (auto &level : scope_uses) {
      for (const Access &other : level) {
        if (!clashes(use, other)) continue;
        const char *mine = use.writes ? "writes" : "reads";
        const char *theirs = other.writes ? "written" : "read";
        error(use.at, "this %s '%s' while it is already %s concurrently",
              mine, use.root->name.c_str(), theirs);
        note(other.at, "the other use is here");
        if (use.root->type && use.root->type->kind == TY_SLICE)
          note(use.root->pos,
               "to give tasks separate pieces, walk it with "
               "'for part in %s.chunks(n)'",
               use.root->name.c_str());
        return;
      }
    }
    if (!scope_uses.empty()) scope_uses.back().push_back(use);
  }

  void walk(Node *n, const std::function<void(Node *)> &visit) {
    if (!n) return;
    visit(n);
    // A spawned call is the task's business, and a partition header names the
    // slice it is about to split; neither is a use by the parent.
    if (n->kind == ND_SPAWN) return;
    bool partition = n->kind == ND_FOR && !n->is_range && !n->name.empty();
    for (Node *kid : n->kids) walk(kid, visit);
    if (!partition) {
      walk(n->lhs, visit);
      walk(n->rhs, visit);
    }
    walk(n->cond, visit);
    walk(n->body, visit);
    walk(n->els, visit);
  }

  // Which occurrences of a name sit under `base[i]`, with `i` the loop
  // variable. Those are the ones no other iteration can reach.
  void collect_indexed(Node *body, Symbol *index, std::set<Node *> &covered) {
    walk(body, [&](Node *e) {
      if (e->kind != ND_INDEX) return;
      if (!e->rhs || e->rhs->kind != ND_IDENT || e->rhs->sym != index) return;
      Node *base = e->lhs;
      while (base && base->kind != ND_IDENT) base = base->lhs;
      if (base) covered.insert(base);
    });
  }

  // Iterations run on different workers at the same time, so whatever the
  // body writes has to be reachable by exactly one of them.
  void check_parallel_body(Node *loop) {
    std::set<Symbol *> local;
    local.insert(loop->sym);
    walk(loop->body, [&](Node *n) {
      if (n->sym && (n->kind == ND_VAR || n->kind == ND_CATCH ||
                     (n->kind == ND_FOR && !n->name.empty()) ||
                     (n->kind == ND_IF && !n->name.empty())))
        local.insert(n->sym);
      if (n->index_sym) local.insert(n->index_sym);
    });

    if (loop->reduce_sym)
      walk(loop->body, [&](Node *n) {
        if (n->kind == ND_VAR && n->name == loop->text)
          error(n->pos,
                "'%s' is the reduction variable; declaring it again here means "
                "the loop accumulates into something nobody reads",
                n->name.c_str());
      });

    std::set<Node *> covered;
    collect_indexed(loop->body, loop->sym, covered);

    std::set<Symbol *> written;
    walk(loop->body, [&](Node *n) {
      if (n->kind != ND_ASSIGN) return;
      Symbol *root = share_root(n->lhs);
      if (!root || local.count(root) || root == loop->reduce_sym) return;
      Node *base = n->lhs;
      while (base && base->kind != ND_IDENT) base = base->lhs;
      if (!base || !covered.count(base)) {
        bool indexable = root->type && (root->type->kind == TY_SLICE ||
                                        root->type->kind == TY_ARRAY);
        if (indexable)
          error(n->lhs->pos,
                "every iteration would write '%s'; a 'parallel for' body may "
                "only write '%s[%s]'",
                root->name.c_str(), root->name.c_str(), loop->name.c_str());
        else
          error(n->lhs->pos,
                "every iteration would write '%s'; to accumulate into it, "
                "write 'reduce(+: %s)' on the loop",
                root->name.c_str(), root->name.c_str());
        return;
      }
      written.insert(root);
    });

    // Once a name is written by the loop, every other mention of it has to be
    // indexed too, or one iteration would read what another is writing.
    walk(loop->body, [&](Node *n) {
      if (n->kind != ND_IDENT || !n->sym || !written.count(n->sym)) return;
      if (covered.count(n)) return;
      error(n->pos,
            "'%s' is written by this loop, so it can only be reached as "
            "'%s[%s]'",
            n->sym->name.c_str(), n->sym->name.c_str(), loop->name.c_str());
    });
  }

  // Everything the parent itself touches inside the block races with the
  // tasks it started, because the join is at the closing brace.
  void check_parent_uses(Node *body) {
    std::set<Node *> written;
    walk(body, [&](Node *n) {
      if (n->kind == ND_ASSIGN) written.insert(n->lhs);
    });
    walk(body, [&](Node *n) {
      if (n->kind == ND_ASSIGN) {
        record_use(access_of(n->lhs, true));
      } else if (n->kind == ND_IDENT && n->sym && !n->sym->is_func &&
                 !written.count(n)) {
        record_use(access_of(n, false));
      }
    });
  }

  // --- generics -----------------------------------------------------------

  // Instance names have to survive into the object file, so the type is
  // flattened to something an identifier can hold.
  static std::string mangle(const Type *t) {
    std::string text = type_str(t);
    std::string out;
    for (char c : text) {
      if (isalnum((unsigned char)c) || c == '_') out += c;
      else if (!out.empty() && out.back() != '.') out += '.';
    }
    return out.empty() ? "t" : out;
  }

  bool is_type_param(Node *decl, const std::string &name) {
    for (Node *tp : decl->tparams)
      if (tp->name == name) return true;
    return false;
  }

  // Matches a written parameter type against the type actually passed,
  // binding type parameters as it goes.
  bool unify(Node *decl, Node *pattern, Type *actual,
             std::vector<std::pair<std::string, Type *>> &bind) {
    if (!pattern || !actual) return false;
    switch (pattern->kind) {
    case ND_TYPE_NAME: {
      if (!pattern->text.empty() || !is_type_param(decl, pattern->name))
        return true; // a concrete type; the ordinary argument check covers it
      for (auto &entry : bind)
        if (entry.first == pattern->name) return type_eq(entry.second, actual);
      bind.push_back({pattern->name, actual});
      return true;
    }
    case ND_TYPE_PTR:
      return actual->kind == TY_PTR && unify(decl, pattern->lhs, actual->elem, bind);
    case ND_TYPE_RAWPTR:
      return actual->kind == TY_RAWPTR && unify(decl, pattern->lhs, actual->elem, bind);
    case ND_TYPE_SLICE:
      return actual->kind == TY_SLICE && unify(decl, pattern->lhs, actual->elem, bind);
    case ND_TYPE_ARRAY:
      return actual->kind == TY_ARRAY && unify(decl, pattern->lhs, actual->elem, bind);
    case ND_TYPE_OPT:
      return is_optional(actual) &&
             unify(decl, pattern->lhs, const_cast<Type *>(opt_payload(actual)), bind);
    case ND_TYPE_ERR:
      return actual->is_error_union &&
             unify(decl, pattern->lhs, actual->elem, bind);
    default:
      return true;
    }
  }

  bool satisfies(Type *arg, Type *constraint, Pos at) {
    if (!constraint->is_interface) {
      error(at, "a constraint must be an interface, got %s",
            type_str(constraint).c_str());
      return false;
    }
    Type *owner = arg->kind == TY_PTR ? arg->elem : arg;
    for (const Field &want : constraint->methods) {
      if (find_method(owner, want.name)) continue;
      error(at, "%s does not satisfy %s: no method '%s'",
            type_str(arg).c_str(), constraint->name.c_str(),
            want.name.c_str());
      return false;
    }
    return true;
  }

  Symbol *instantiate(Node *generic, Package *owner,
                      const std::vector<Type *> &args, Pos at) {
    if (args.size() != generic->tparams.size()) {
      error(at, "'%s' takes %zu type argument%s, got %zu",
            generic->name.c_str(), generic->tparams.size(),
            generic->tparams.size() == 1 ? "" : "s", args.size());
      return nullptr;
    }

    std::string key = generic->name;
    for (Type *a : args) key += "$" + mangle(a);
    auto found = prog.instances.find(key);
    if (found != prog.instances.end()) return found->second;

    // A generic that calls itself on an ever-larger type would instantiate
    // forever; stop with a diagnostic instead of hanging.
    if (prog.instances.size() >= 4096) {
      error(at, "'%s' keeps instantiating new types; the recursion has no base "
                "case", generic->name.c_str());
      return nullptr;
    }

    std::vector<std::pair<std::string, Type *>> bind;
    for (size_t i = 0; i < args.size(); i++)
      bind.push_back({generic->tparams[i]->name, args[i]});

    // The signature is written in the package that declares the generic, so
    // it has to be resolved with that package's names in view, not the
    // caller's.
    Checker sub(prog, *owner, types);
    sub.bindings = bind;

    for (size_t i = 0; i < args.size(); i++) {
      Node *tp = generic->tparams[i];
      if (!tp->type_expr) continue;
      if (!satisfies(args[i], sub.resolve(tp->type_expr), at)) return nullptr;
    }

    Node *copy = ast.clone(generic);
    copy->tparams.clear();
    copy->name = key;

    std::vector<Type *> params;
    for (Node *p : copy->kids) {
      p->type = sub.resolve(p->type_expr);
      params.push_back(p->type);
    }
    Type *ret = copy->type_expr ? sub.resolve(copy->type_expr) : types.void_ty;

    Symbol *sym = ast.make_symbol(key, types.func(params, ret), false,
                                  generic->pos);
    sym->is_func = true;
    sym->decl = copy;
    copy->sym = sym;

    prog.instances[key] = sym;
    owner->instances.push_back(copy);
    prog.pending.push_back(Program::Instance{owner, copy, bind});
    return sym;
  }

  // `sizeof[T]()` and `alignof[T]()` are the two things a generic allocator
  // cannot compute for itself.
  bool builtin_size(Node *n, const std::string &name,
                    const std::vector<Type *> &args) {
    if (name != "sizeof" && name != "alignof") return false;
    if (args.size() != 1 || !n->kids.empty()) {
      error(n->pos, "'%s' takes one type argument and no values",
            name.c_str());
      return true;
    }
    n->kind = ND_INT_LIT;
    n->ival = name == "sizeof" ? (uint64_t)size_of(args[0])
                               : (uint64_t)align_of(args[0]);
    n->type = types.untyped_int;
    return true;
  }

  // --- interfaces ---------------------------------------------------------

  // A `*T` becomes an interface value by pairing the pointer with a vtable
  // built for that exact (type, interface) combination. Only pointers qualify:
  // boxing a value would mean allocating, and nothing allocates implicitly.
  bool bind_interface(Node *value, Type *iface) {
    Type *from = value->type;
    if (!from || from->kind != TY_PTR || from->elem->kind != TY_STRUCT ||
        from->elem->is_interface) {
      note(value->pos, "%s is satisfied by a pointer to a struct",
           iface->name.c_str());
      return false;
    }
    Type *owner = from->elem;

    std::vector<std::string> entries;
    for (const Field &want : iface->methods) {
      Symbol *have = find_method(owner, want.name);
      if (!have) {
        note(value->pos, "%s has no method '%s'", owner->name.c_str(),
             want.name.c_str());
        return false;
      }
      Type *got = have->type;
      bool same = type_eq(got->ret, want.type->ret) &&
                  got->params.size() == want.type->params.size() + 1;
      for (size_t i = 0; same && i < want.type->params.size(); i++)
        same = type_eq(got->params[i + 1], want.type->params[i]);
      if (!same) {
        note(value->pos, "%s.%s is %s, but %s wants %s",
             owner->name.c_str(), want.name.c_str(), type_str(got).c_str(),
             iface->name.c_str(), type_str(want.type).c_str());
        return false;
      }
      entries.push_back(have->name);
    }

    value->vtable = types.vtable(owner->name + ">" + iface->name, entries);
    value->bind_to = iface;
    return true;
  }

  // Every place a value flows into a declared type goes through here, so
  // interface conversion happens uniformly.
  bool convert(Node *value, Type *to) {
    if (assignable(value->type, to)) {
      apply_type(value, to);
      return true;
    }
    return to->is_interface && bind_interface(value, to);
  }

  // --- expressions -------------------------------------------------------

  Type *check_binary(Node *n) {
    Type *lhs = check_expr(n->lhs);
    Type *rhs = check_expr(n->rhs);
    if (!lhs || !rhs) return nullptr;

    if (n->op == TK_ANDAND || n->op == TK_OROR) {
      if (lhs->kind != TY_BOOL || rhs->kind != TY_BOOL) {
        error(n->pos, "'%s' needs bool operands, got %s and %s",
              tok_name(n->op), type_str(lhs).c_str(), type_str(rhs).c_str());
        return nullptr;
      }
      return n->type = types.bool_ty;
    }

    // A raw pointer can be offset. `*T` and `[]T` deliberately cannot: they
    // are the checked side of the boundary.
    if ((n->op == TK_PLUS || n->op == TK_MINUS) && lhs->kind == TY_RAWPTR &&
        is_integer(rhs)) {
      apply_type(n->rhs, types.usize_ty);
      settle(n->rhs);
      return n->type = lhs;
    }

    // Whichever side is concrete decides the type of the other.
    if (lhs->untyped && assignable(lhs, rhs)) { apply_type(n->lhs, rhs); lhs = rhs; }
    else if (rhs->untyped && assignable(rhs, lhs)) { apply_type(n->rhs, lhs); rhs = lhs; }

    if (!type_eq(lhs, rhs)) {
      error(n->pos, "mismatched operands for '%s': %s and %s",
            tok_name(n->op), type_str(lhs).c_str(), type_str(rhs).c_str());
      return nullptr;
    }

    bool is_ptr = lhs->kind == TY_PTR || lhs->kind == TY_RAWPTR;
    switch (n->op) {
    case TK_EQ: case TK_NE:
      if (!is_numeric(lhs) && lhs->kind != TY_BOOL && !is_ptr) {
        error(n->pos, "cannot compare values of type %s",
              type_str(lhs).c_str());
        return nullptr;
      }
      return n->type = types.bool_ty;
    case TK_LT: case TK_LE: case TK_GT: case TK_GE:
      if (!is_numeric(lhs)) {
        error(n->pos, "cannot order values of type %s",
              type_str(lhs).c_str());
        return nullptr;
      }
      return n->type = types.bool_ty;
    case TK_PERCENT: case TK_SHL: case TK_SHR:
    case TK_AMP: case TK_PIPE: case TK_CARET:
      if (!is_integer(lhs)) {
        error(n->pos, "'%s' needs integer operands, got %s",
              tok_name(n->op), type_str(lhs).c_str());
        return nullptr;
      }
      return n->type = lhs;
    default:
      if (!is_numeric(lhs)) {
        error(n->pos, "'%s' needs numeric operands, got %s",
              tok_name(n->op), type_str(lhs).c_str());
        return nullptr;
      }
      return n->type = lhs;
    }
  }

  // `i32(x)` parses as a call; if the name is a type and nothing shadows it,
  // it is a conversion instead. Same rule as Go.
  Type *check_convert(Node *n, Type *target) {
    n->kind = ND_CONVERT;
    if (n->kids.size() != 1) {
      error(n->pos, "%s(...) converts exactly one value, got %zu",
            type_str(target).c_str(), n->kids.size());
      return nullptr;
    }
    Type *from = check_expr(n->kids[0]);
    if (!from) return nullptr;
    apply_type(n->kids[0], target);
    from = settle(n->kids[0]);

    bool ok = (is_numeric(from) && is_numeric(target)) ||
              // Raw pointers are the unchecked side of the boundary, so they
              // reinterpret freely. `*T` converts out to one, never back in:
              // that would fabricate a non-null guarantee.
              (from->kind == TY_RAWPTR && target->kind == TY_RAWPTR) ||
              (from->kind == TY_PTR && target->kind == TY_RAWPTR);
    if (!ok) {
      error(n->pos, "cannot convert %s to %s", type_str(from).c_str(),
            type_str(target).c_str());
      return nullptr;
    }
    return n->type = target;
  }

  Symbol *find_method(Type *owner, const std::string &name) {
    if (!owner || owner->kind != TY_STRUCT) return nullptr;
    auto by_type = prog.methods.find(owner->name);
    if (by_type == prog.methods.end()) return nullptr;
    auto found = by_type->second.find(name);
    return found == by_type->second.end() ? nullptr : found->second;
  }

  // Checks the argument list against a signature, skipping `skip` leading
  // parameters that the call site fills in for itself (the receiver).
  bool check_args(Node *n, Symbol *sym, size_t skip,
                  bool already_checked = false) {
    Type *sig = sym->type;
    size_t wanted = sig->params.size() - skip;
    if (n->kids.size() != wanted) {
      error(n->pos, "'%s' takes %zu argument%s, got %zu",
            sym->name.c_str(), wanted, wanted == 1 ? "" : "s", n->kids.size());
      return false;
    }
    for (size_t i = 0; i < n->kids.size(); i++) {
      Type *want = sig->params[i + skip];
      Type *arg = already_checked ? n->kids[i]->type : check_expr(n->kids[i]);
      if (!arg) return false;
      if (!convert(n->kids[i], want)) {
        error(n->kids[i]->pos, "argument %zu of '%s' expects %s, got %s",
              i + 1, sym->name.c_str(), type_str(want).c_str(),
              type_str(arg).c_str());
        return false;
      }

      // A `mut` parameter hands the callee write access, so the caller must
      // be entitled to it in the first place.
      Node *param = sym->decl ? sym->decl->kids[i + skip] : nullptr;
      if (param && param->is_mut && !require_mutable(n->kids[i], "pass"))
        return false;
    }
    return true;
  }

  Type *check_method_call(Node *n) {
    Node *field = n->lhs;
    Type *base = check_expr(field->lhs);
    if (!base) return nullptr;
    Type *owner = base->kind == TY_PTR ? base->elem : base;

    if (owner->is_interface) {
      for (size_t i = 0; i < owner->methods.size(); i++) {
        const Field &m = owner->methods[i];
        if (m.name != field->name) continue;
        n->form = 2; // dynamic: the function pointer comes from the vtable
        n->ival = i;
        Symbol probe{field->name, m.type,   false, false,   false,
                     nullptr,     field->pos, -1,    nullptr};
        if (!check_args(n, &probe, 0)) return nullptr;
        return n->type = m.type->ret;
      }
      error(field->pos, "%s has no method '%s'", owner->name.c_str(),
            field->name.c_str());
      return nullptr;
    }

    Symbol *sym = find_method(owner, field->name);
    if (!sym) {
      error(field->pos, "%s has no method '%s'", type_str(owner).c_str(),
            field->name.c_str());
      return nullptr;
    }
    if (!is_place(field->lhs) && base->kind != TY_PTR) {
      error(field->pos,
            "a method needs a value with an address; bind it first");
      return nullptr;
    }

    Node *recv = sym->decl->kids[0];
    if (recv->is_mut && !require_mutable(field->lhs, "call a mutating method on"))
      return nullptr;

    n->form = 1; // method: the receiver is passed ahead of the arguments
    n->name = sym->name;
    n->sym = sym;
    if (!check_args(n, sym, 1)) return nullptr;
    return n->type = sym->type->ret;
  }

  // `mem.init(...)` looks exactly like a method call until you notice that
  // `mem` names a package rather than a value.
  Type *check_package_call(Node *n, Package *other) {
    Node *field = n->lhs;
    Symbol *sym = find_in(other->globals, field->name, false);
    if (!sym) {
      error(field->pos, "package '%s' has no exported function '%s'",
            other->name.c_str(), field->name.c_str());
      return nullptr;
    }
    if (!sym->is_func) {
      error(field->pos, "'%s.%s' is not a function", other->name.c_str(),
            field->name.c_str());
      return nullptr;
    }
    if (sym->is_generic) return check_generic_call(n, sym, other, {});
    n->sym = sym;
    n->name = sym->name;
    if (!check_args(n, sym, 0)) return nullptr;
    return n->type = sym->type->ret;
  }

  Type *type_from_expr(Node *e) {
    if (e->kind == ND_IDENT) {
      Node probe = *e;
      probe.kind = ND_TYPE_NAME;
      probe.text.clear();
      return lookup_type(&probe);
    }
    if (e->kind == ND_FIELD && e->lhs->kind == ND_IDENT) {
      Node probe = *e;
      probe.kind = ND_TYPE_NAME;
      probe.text = e->lhs->name;
      probe.name = e->name;
      return lookup_type(&probe);
    }
    error(e->pos, "expected a type argument");
    return nullptr;
  }

  Type *check_generic_call(Node *n, Symbol *sym, Package *owner,
                           std::vector<Type *> args) {
    Node *generic = sym->decl;

    // No explicit type arguments: read them off the values being passed.
    bool checked = false;
    if (args.empty()) {
      if (n->kids.size() != generic->kids.size()) {
        error(n->pos, "'%s' takes %zu argument%s, got %zu",
              generic->name.c_str(), generic->kids.size(),
              generic->kids.size() == 1 ? "" : "s", n->kids.size());
        return nullptr;
      }
      std::vector<std::pair<std::string, Type *>> bind;
      for (size_t i = 0; i < n->kids.size(); i++) {
        if (!check_expr(n->kids[i])) return nullptr;
        unify(generic, generic->kids[i]->type_expr, settle(n->kids[i]), bind);
      }
      checked = true;
      for (Node *tp : generic->tparams) {
        Type *found = nullptr;
        for (auto &entry : bind)
          if (entry.first == tp->name) found = entry.second;
        if (!found) {
          error(n->pos,
                "cannot infer '%s' for '%s'; pass it as '%s[Type](...)'",
                tp->name.c_str(), generic->name.c_str(),
                generic->name.c_str());
          return nullptr;
        }
        args.push_back(found);
      }
    }

    Symbol *inst = instantiate(generic, owner, args, n->pos);
    if (!inst) return nullptr;
    n->sym = inst;
    n->name = inst->name;
    if (!check_args(n, inst, 0, checked)) return nullptr;
    return n->type = inst->type->ret;
  }

  // `f[i32](x)` parses as a call on an index expression; the index is really
  // a type argument list.
  Type *check_indexed_call(Node *n) {
    Node *index = n->lhs;
    Node *callee = index->lhs;

    std::vector<Node *> arg_exprs{index->rhs};
    for (Node *more : index->kids) arg_exprs.push_back(more);

    Symbol *sym = nullptr;
    Package *owner = &pkg;
    std::string shown;
    if (callee->kind == ND_IDENT) {
      shown = callee->name;
      sym = lookup(callee->name);
      if (!sym) {
        std::vector<Type *> args;
        for (Node *e : arg_exprs) {
          Type *a = type_from_expr(e);
          if (!a) return nullptr;
          args.push_back(a);
        }
        if (builtin_size(n, callee->name, args)) return n->type;
      }
    } else if (callee->kind == ND_FIELD && callee->lhs->kind == ND_IDENT &&
               !lookup(callee->lhs->name)) {
      owner = imported(callee->lhs->name);
      shown = callee->name;
      if (owner) sym = find_in(owner->globals, callee->name, false);
    }

    if (!sym || !sym->is_generic) {
      error(n->pos, "'%s' is not a generic function", shown.c_str());
      return nullptr;
    }

    std::vector<Type *> args;
    for (Node *e : arg_exprs) {
      Type *a = type_from_expr(e);
      if (!a) return nullptr;
      args.push_back(a);
    }
    return check_generic_call(n, sym, owner, args);
  }

  Type *check_call(Node *n) {
    if (n->lhs->kind == ND_INDEX) return check_indexed_call(n);
    if (n->lhs->kind == ND_FIELD) {
      Node *base = n->lhs->lhs;
      if (base->kind == ND_IDENT && !lookup(base->name))
        if (Package *other = imported(base->name))
          return check_package_call(n, other);
      return check_method_call(n);
    }
    if (n->lhs->kind != ND_IDENT) {
      error(n->pos, "only direct calls are supported for now");
      return nullptr;
    }
    if (!lookup(n->lhs->name))
      if (Type *target = lookup_type(n->lhs->name))
        return check_convert(n, target);
    Symbol *sym = lookup(n->lhs->name);
    if (!sym) {
      error(n->lhs->pos, "undefined function '%s'", n->lhs->name.c_str());
      return nullptr;
    }
    if (!sym->is_func) {
      error(n->lhs->pos, "'%s' is not a function", sym->name.c_str());
      return nullptr;
    }
    n->lhs->sym = sym;

    if (sym->is_generic) return check_generic_call(n, sym, &pkg, {});
    n->sym = sym;
    n->name = sym->name;
    if (!check_args(n, sym, 0)) return nullptr;
    return n->type = sym->type->ret;
  }

  Type *check_field(Node *n) {
    // `error.Name` parses as a field access. Nothing else is spelled that way,
    // so intercept it before the base is resolved as an expression.
    if (n->lhs->kind == ND_IDENT && n->lhs->name == "error" &&
        !lookup("error")) {
      n->kind = ND_ERROR_LIT;
      n->ival = (uint64_t)types.error_code(n->name);
      return n->type = types.error_ty;
    }

    Type *base = check_expr(n->lhs);
    if (!base) return nullptr;
    if (base->is_error_union) {
      error(n->pos, "%s must be handled with 'try' or 'catch' first",
            type_str(base).c_str());
      return nullptr;
    }

    // A field reached through a pointer dereferences automatically.
    if (base->kind == TY_PTR) base = base->elem;

    if (base->kind == TY_SLICE || base->kind == TY_STRING ||
        base->kind == TY_ARRAY) {
      if (n->name == "len") return n->type = types.usize_ty;
      if (n->name == "ptr" && base->kind != TY_ARRAY)
        return n->type = types.rawptr(base->kind == TY_STRING ? types.u8_ty
                                                             : base->elem);
      error(n->pos, "%s has no field '%s'", type_str(base).c_str(),
            n->name.c_str());
      return nullptr;
    }

    const Field *field = find_field(base, n->name);
    if (!field) {
      error(n->pos, "%s has no field '%s'", type_str(base).c_str(),
            n->name.c_str());
      return nullptr;
    }
    return n->type = field->type;
  }

  Type *element_of(Type *base) {
    switch (base->kind) {
    case TY_ARRAY: case TY_SLICE: case TY_RAWPTR: return base->elem;
    case TY_STRING: return types.u8_ty;
    default: return nullptr;
    }
  }

  Type *check_index(Node *n) {
    Type *base = check_expr(n->lhs);
    Type *index = check_expr(n->rhs);
    if (!base || !index) return nullptr;
    if (base->kind == TY_PTR) base = base->elem;

    Type *elem = element_of(base);
    if (!elem) {
      error(n->pos, "cannot index a value of type %s",
            type_str(base).c_str());
      return nullptr;
    }
    if (!is_integer(index)) {
      error(n->rhs->pos, "index must be an integer, got %s",
            type_str(index).c_str());
      return nullptr;
    }
    apply_type(n->rhs, types.usize_ty);
    settle(n->rhs);
    n->lhs->type = base;
    return n->type = elem;
  }

  Type *check_slice_expr(Node *n) {
    Type *base = check_expr(n->lhs);
    if (!base) return nullptr;
    if (base->kind == TY_PTR) base = base->elem;

    Type *elem = element_of(base);
    if (!elem) {
      error(n->pos, "cannot slice a value of type %s", type_str(base).c_str());
      return nullptr;
    }
    if (base->kind == TY_RAWPTR && (!n->cond || !n->rhs)) {
      error(n->pos,
            "slicing %s needs both bounds: it has no length of its own",
            type_str(base).c_str());
      return nullptr;
    }
    for (Node *bound : {n->cond, n->rhs}) {
      if (!bound) continue;
      Type *t = check_expr(bound);
      if (!t) return nullptr;
      if (!is_integer(t)) {
        error(bound->pos, "slice bound must be an integer, got %s",
              type_str(t).c_str());
        return nullptr;
      }
      apply_type(bound, types.usize_ty);
      settle(bound);
    }
    n->lhs->type = base;
    return n->type = base->kind == TY_STRING ? types.string_ty
                                             : types.slice(elem);
  }

  Type *check_struct_lit(Node *n) {
    Type *type = lookup_type(n->name);
    if (!type || type->kind != TY_STRUCT || type->is_interface) {
      error(n->pos, "'%s' is not a struct type", n->name.c_str());
      return nullptr;
    }

    std::unordered_set<std::string> seen;
    for (Node *init : n->kids) {
      const Field *field = find_field(type, init->name);
      if (!field) {
        error(init->pos, "%s has no field '%s'", n->name.c_str(),
              init->name.c_str());
        return nullptr;
      }
      if (!seen.insert(init->name).second) {
        error(init->pos, "field '%s' is set twice", init->name.c_str());
        return nullptr;
      }
      Type *value = check_expr(init->rhs);
      if (!value) return nullptr;
      if (!convert(init->rhs, field->type)) {
        error(init->rhs->pos, "field '%s' expects %s, got %s",
              init->name.c_str(), type_str(field->type).c_str(),
              type_str(value).c_str());
        return nullptr;
      }
      init->type = field->type;
    }

    for (const Field &field : type->fields) {
      if (seen.count(field.name)) continue;
      error(n->pos, "field '%s' of %s is not initialized",
            field.name.c_str(), n->name.c_str());
      return nullptr;
    }
    return n->type = type;
  }

  Type *check_array_lit(Node *n) {
    Node *spec = n->type_expr;
    if (spec->kind != ND_TYPE_ARRAY) {
      // `[]T{...}` would have to allocate, and Sword has no hidden allocation.
      error(n->pos,
            "array literals need a fixed length; write [%zu]T{...} and slice it",
            n->kids.size());
      return nullptr;
    }

    Type *elem = resolve(spec->lhs);
    int64_t count = spec->ival == (uint64_t)-1 ? (int64_t)n->kids.size()
                                               : (int64_t)spec->ival;
    // `[64]u8{}` is the zeroed buffer; anything else must be spelled out.
    if (n->kids.empty() && count > 0) return n->type = types.array(elem, count);
    if ((size_t)count != n->kids.size()) {
      error(n->pos, "array of length %lld initialized with %zu elements",
            (long long)count, n->kids.size());
      return nullptr;
    }

    for (Node *item : n->kids) {
      Type *t = check_expr(item);
      if (!t) return nullptr;
      if (!assignable(t, elem)) {
        error(item->pos, "array element expects %s, got %s",
              type_str(elem).c_str(), type_str(t).c_str());
        return nullptr;
      }
      apply_type(item, elem);
    }
    return n->type = types.array(elem, count);
  }

  Type *check_unary(Node *n) {
    if (n->op == TK_AMP) {
      Type *operand = check_expr(n->lhs);
      if (!operand) return nullptr;
      if (!is_place(n->lhs)) {
        error(n->pos, "cannot take the address of this expression");
        return nullptr;
      }
      return n->type = types.ptr(operand);
    }

    Type *operand = check_expr(n->lhs);
    if (!operand) return nullptr;

    switch (n->op) {
    case TK_STAR:
      if (operand->kind != TY_PTR && operand->kind != TY_RAWPTR) {
        error(n->pos, "cannot dereference a value of type %s",
              type_str(operand).c_str());
        return nullptr;
      }
      return n->type = operand->elem;
    case TK_BANG:
      if (operand->kind != TY_BOOL) {
        error(n->pos, "'!' needs a bool, got %s",
              type_str(operand).c_str());
        return nullptr;
      }
      return n->type = types.bool_ty;
    case TK_MINUS:
      if (!is_numeric(operand)) {
        error(n->pos, "'-' needs a numeric value, got %s",
              type_str(operand).c_str());
        return nullptr;
      }
      return n->type = operand;
    default:
      error(n->pos, "operator '%s' is not implemented yet",
            tok_name(n->op));
      return nullptr;
    }
  }

  Type *check_try(Node *n) {
    if (!open_scopes.empty()) {
      error(n->pos, "'try' inside a 'scope' would leave without joining; do "
                    "the fallible work before the scope, or use 'catch'");
      return nullptr;
    }
    Type *t = check_expr(n->lhs);
    if (!t) return nullptr;
    if (!t->is_error_union) {
      error(n->pos, "'try' needs a value that can fail, got %s",
            type_str(t).c_str());
      return nullptr;
    }
    if (!ret_type->is_error_union) {
      error(n->pos,
            "'try' propagates an error, so this function must return '!'");
      return nullptr;
    }
    return n->type = t->elem;
  }

  Type *check_catch(Node *n) {
    Type *t = check_expr(n->lhs);
    if (!t) return nullptr;
    if (!t->is_error_union) {
      error(n->pos, "'catch' needs a value that can fail, got %s",
            type_str(t).c_str());
      return nullptr;
    }
    Type *value = t->elem;

    if (n->body) {
      push_scope();
      if (!n->name.empty())
        n->sym = declare(n->name, types.error_ty, false, n->pos);
      check_stmt(n->body);
      pop_scope();
      // The block produces no value, so when one is expected it has to leave
      // rather than fall out. With nothing to produce, falling out is fine.
      if (value->kind != TY_VOID && n->form != 1 && !always_leaves(n->body)) {
        error(n->pos,
              "this 'catch' block must return, break or continue: %s is "
              "expected here",
              type_str(value).c_str());
        return nullptr;
      }
      return n->type = value;
    }

    Type *fallback = check_expr(n->rhs);
    if (!fallback) return nullptr;
    if (!assignable(fallback, value)) {
      error(n->rhs->pos, "'catch' must produce %s, got %s",
            type_str(value).c_str(), type_str(fallback).c_str());
      return nullptr;
    }
    apply_type(n->rhs, value);
    return n->type = value;
  }

  Type *check_orelse(Node *n) {
    Type *t = check_expr(n->lhs);
    if (!t) return nullptr;
    if (!is_optional(t)) {
      error(n->pos, "'orelse' needs an optional value, got %s",
            type_str(t).c_str());
      return nullptr;
    }
    Type *value = const_cast<Type *>(opt_payload(t));

    if (n->body) {
      push_scope();
      check_stmt(n->body);
      pop_scope();
      if (value->kind != TY_VOID && n->form != 1 && !always_leaves(n->body)) {
        error(n->pos,
              "this 'orelse' block must return, break or continue: %s is "
              "expected here",
              type_str(value).c_str());
        return nullptr;
      }
      return n->type = value;
    }

    Type *fallback = check_expr(n->rhs);
    if (!fallback) return nullptr;
    if (!assignable(fallback, value)) {
      error(n->rhs->pos, "'orelse' must produce %s, got %s",
            type_str(value).c_str(), type_str(fallback).c_str());
      return nullptr;
    }
    apply_type(n->rhs, value);
    return n->type = value;
  }

  Type *check_expr(Node *n) {
    if (!n) return nullptr;
    switch (n->kind) {
    case ND_ERROR_LIT: return n->type = types.error_ty;
    case ND_NIL_LIT: return n->type = types.untyped_nil;
    case ND_CONVERT: return check_convert(n, resolve(n->type_expr));
    case ND_ORELSE: return check_orelse(n);
    case ND_TRY: return check_try(n);
    case ND_CATCH: return check_catch(n);
    case ND_INT_LIT: return n->type = types.untyped_int;
    case ND_BOOL_LIT: return n->type = types.bool_ty;
    case ND_STRING_LIT: return n->type = types.string_ty;
    case ND_ARRAY_LIT: return check_array_lit(n);
    case ND_STRUCT_LIT: return check_struct_lit(n);
    case ND_IDENT: {
      Symbol *sym = lookup(n->name);
      if (!sym) {
        error(n->pos, "undefined identifier '%s'", n->name.c_str());
        return nullptr;
      }
      n->sym = sym;
      return n->type = sym->type;
    }
    case ND_BINARY: return check_binary(n);
    case ND_UNARY: return check_unary(n);
    case ND_CALL: return check_call(n);
    case ND_FIELD: return check_field(n);
    case ND_INDEX: return check_index(n);
    case ND_SLICE_EXPR: return check_slice_expr(n);
    default:
      error(n->pos, "unexpected expression");
      return nullptr;
    }
  }

  // --- statements --------------------------------------------------------

  void check_condition(Node *n, const char *what) {
    Type *t = check_expr(n);
    if (t && t->kind != TY_BOOL)
      error(n->pos, "%s condition must be bool, got %s", what,
            type_str(t).c_str());
  }

  void check_assign(Node *n) {
    Type *target = check_expr(n->lhs);
    Type *value = check_expr(n->rhs);
    if (!target || !value) return;

    if (!is_place(n->lhs)) {
      error(n->lhs->pos, "cannot assign to this expression");
      return;
    }
    if (!require_mutable(n->lhs, "assign to")) return;

    if (n->op != TK_ASSIGN && !is_numeric(target)) {
      error(n->pos, "'%s' needs a numeric target, got %s",
            tok_name(n->op), type_str(target).c_str());
      return;
    }
    if (!convert(n->rhs, target)) {
      error(n->rhs->pos, "cannot assign %s to %s",
            type_str(value).c_str(), type_str(target).c_str());
      return;
    }
  }

  // A `return` in a fallible function has four shapes; which one it is decides
  // what lowering writes into the { code, value } pair.
  enum ReturnForm { RET_PLAIN, RET_WRAP, RET_ERROR, RET_FORWARD, RET_OK };

  void check_return(Node *n) {
    if (!ret_type->is_error_union) {
      n->form = RET_PLAIN;
      if (n->lhs) {
        Type *t = check_expr(n->lhs);
        if (!t) return;
        if (!convert(n->lhs, ret_type)) {
          error(n->lhs->pos,
                "cannot return %s from a function returning %s",
                type_str(t).c_str(), type_str(ret_type).c_str());
          return;
        }
      } else if (ret_type->kind != TY_VOID) {
        error(n->pos, "this function must return %s",
              type_str(ret_type).c_str());
      }
      return;
    }

    Type *payload = ret_type->elem;
    if (!n->lhs) {
      if (payload->kind != TY_VOID)
        error(n->pos, "this function must return %s",
              type_str(ret_type).c_str());
      n->form = RET_OK;
      return;
    }

    Type *t = check_expr(n->lhs);
    if (!t) return;
    if (type_eq(t, types.error_ty)) {
      n->form = RET_ERROR;
    } else if (type_eq(t, ret_type)) {
      n->form = RET_FORWARD;
    } else if (convert(n->lhs, payload)) {
      n->form = RET_WRAP;
    } else {
      error(n->lhs->pos, "cannot return %s from a function returning %s",
            type_str(t).c_str(), type_str(ret_type).c_str());
    }
  }

  // A task is a call and nothing else: everything it can reach is written on
  // the spawn line, which is what makes the sharing rules checkable.
  void check_spawn(Node *n) {
    if (open_scopes.empty() || open_scopes.back()->kind != ND_SCOPE) {
      error(n->pos, "'spawn' only exists directly inside a 'scope'");
      return;
    }
    if (n->lhs->kind != ND_CALL) {
      error(n->pos, "'spawn' takes a function call");
      return;
    }

    Type *result = check_expr(n->lhs);
    if (!result) return;
    if (n->lhs->form != 0) {
      error(n->pos, "'spawn' cannot call a method yet; call a function");
      return;
    }

    bool fails = result->is_error_union;
    if (fails && result->elem->kind != TY_VOID) {
      error(n->pos,
            "a spawned function cannot return a value, only '!void'; write "
            "the result into memory the caller owns");
      return;
    }
    if (!fails && result->kind != TY_VOID) {
      error(n->pos,
            "a spawned function cannot return a value; write it into memory "
            "the caller owns");
      return;
    }
    if (fails) open_scopes.back()->form = 1;

    // What the task can reach is exactly what is written on this line, and
    // `mut` on the parameter says whether it may write.
    bool in_loop = !scope_loop_base.empty() &&
                   loop_depth > scope_loop_base.back();
    Symbol *callee = n->lhs->sym;
    for (size_t i = 0; i < n->lhs->kids.size(); i++) {
      Type *param = callee->type->params[i];
      if (!shares_memory(param)) continue;
      Node *decl = callee->decl ? callee->decl->kids[i] : nullptr;
      Access use = access_of(n->lhs->kids[i], decl && decl->is_mut);
      use.repeated = in_loop;
      record_use(use);
    }
  }

  // `for part in xs.chunks(n)` is the only way to hand pieces of one slice to
  // different tasks: the pieces cannot overlap, so the compiler does not have
  // to prove anything about the indices.
  bool check_partition(Node *n) {
    Node *call = n->lhs;
    bool shaped = call->kind == ND_CALL && call->lhs->kind == ND_FIELD &&
                  call->lhs->name == "chunks" && call->kids.size() == 1;
    if (!shaped) {
      error(n->lhs->pos,
            "'for %s in ...' walks a partition, written 'xs.chunks(n)'; for a "
            "range write 'for %s in a..b'",
            n->name.c_str(), n->name.c_str());
      return false;
    }

    Node *base = call->lhs->lhs;
    Type *type = check_expr(base);
    if (!type) return false;
    if (type->kind != TY_SLICE) {
      error(base->pos, "chunks() needs a slice, got %s", type_str(type).c_str());
      return false;
    }
    if (!is_place(base)) {
      error(base->pos, "chunks() needs a slice with a name behind it");
      return false;
    }

    Node *size = call->kids[0];
    Type *size_type = check_expr(size);
    if (!size_type) return false;
    if (!is_integer(size_type)) {
      error(size->pos, "chunk size must be an integer, got %s",
            type_str(size_type).c_str());
      return false;
    }
    apply_type(size, types.usize_ty);
    settle(size);

    // Flatten the call into base and size so lowering sees a plain loop.
    n->lhs = base;
    n->rhs = size;
    n->type = type;

    if (!n->text.empty()) {
      error(n->pos, "a partition loop has no reduction");
      return false;
    }

    Symbol *root = place_root(base);
    n->sym = declare(n->name, type, root && root->is_mut, n->pos);
    n->sym->chunk_of = root;
    if (!n->name2.empty())
      n->index_sym = declare(n->name2, types.usize_ty, false, n->pos);
    return true;
  }

  // Each worker accumulates into a private copy, so the operator has to have
  // an identity and combine associatively.
  bool check_reduction(Node *n) {
    Symbol *sym = lookup(n->text);
    if (!sym) {
      error(n->pos, "undefined identifier '%s' in the reduction",
            n->text.c_str());
      return false;
    }
    if (!sym->is_mut) {
      error(n->pos, "'%s' is written by the reduction, so it must be 'mut'",
            n->text.c_str());
      note(sym->pos, "declare it with 'mut' to allow mutation");
      return false;
    }
    if (!is_integer(sym->type)) {
      error(n->pos, "a reduction needs an integer, got %s",
            type_str(sym->type).c_str());
      return false;
    }
    if (n->op != TK_PLUS) {
      error(n->pos, "only '+' reductions exist so far, got '%s'",
            tok_name(n->op));
      return false;
    }
    n->reduce_sym = sym;
    return true;
  }

  void check_stmt(Node *n) {
    if (!n) return;
    switch (n->kind) {
    case ND_BLOCK:
      push_scope();
      for (Node *st : n->kids) check_stmt(st);
      pop_scope();
      break;

    case ND_VAR: {
      Type *declared = n->type_expr ? resolve(n->type_expr) : nullptr;
      Type *t = check_expr(n->rhs);
      if (!t) return;
      if (declared) {
        if (!convert(n->rhs, declared)) {
          error(n->rhs->pos, "cannot initialize %s with %s",
                type_str(declared).c_str(), type_str(t).c_str());
          return;
        }
        t = declared;
      } else {
        if (t->kind == TY_VOID) {
          error(n->pos, "'%s' cannot be bound to a void value",
                n->name.c_str());
          return;
        }
        if (t->kind == TY_OPT && t->untyped) {
          error(n->pos,
                "'%s := nil' has no type to infer; declare it, as in "
                "'%s ?*T = nil'",
                n->name.c_str(), n->name.c_str());
          return;
        }
        t = settle(n->rhs);
      }
      n->type = t;
      n->sym = declare(n->name, t, n->is_mut, n->pos);
      break;
    }

    case ND_ASSIGN:
      check_assign(n);
      break;

    case ND_RETURN:
      if (leaving_scope(n, "return")) return;
      check_return(n);
      break;

    case ND_SCOPE:
      open_scopes.push_back(n);
      scope_loop_base.push_back(loop_depth);
      scope_uses.emplace_back();
      check_stmt(n->body);
      check_parent_uses(n->body);
      scope_uses.pop_back();
      scope_loop_base.pop_back();
      open_scopes.pop_back();
      // A scope that can observe a failure has to have somewhere to report it.
      if (n->form == 1 && !ret_type->is_error_union)
        error(n->pos,
              "this 'scope' can fail, so the function around it must return '!'");
      break;

    case ND_SPAWN:
      check_spawn(n);
      break;

    case ND_DEFER:
      check_stmt(n->body);
      if (n->is_errdefer && !ret_type->is_error_union)
        error(n->pos,
              "'errdefer' only makes sense where an error can happen");
      break;

    case ND_IF:
      // `if x := opt { ... }` runs the body with the unwrapped value bound.
      if (!n->name.empty()) {
        Type *t = check_expr(n->cond);
        if (!t) return;
        if (!is_optional(t)) {
          error(n->cond->pos,
                "'if %s := ...' needs an optional value, got %s",
                n->name.c_str(), type_str(t).c_str());
          return;
        }
        push_scope();
        n->sym = declare(n->name, const_cast<Type *>(opt_payload(t)), false,
                         n->pos);
        check_stmt(n->body);
        pop_scope();
        check_stmt(n->els);
        break;
      }
      check_condition(n->cond, "if");
      check_stmt(n->body);
      check_stmt(n->els);
      break;

    case ND_FOR:
      push_scope();
      if (!n->is_range && !n->name.empty()) {
        if (!check_partition(n)) {
          pop_scope();
          return;
        }
        loop_depth++;
        check_stmt(n->body);
        loop_depth--;
        pop_scope();
        break;
      }
      if (n->is_parallel && !n->is_range) {
        error(n->pos, "'parallel for' needs a range, as in 'for i in 0..n'");
        pop_scope();
        return;
      }
      if (n->is_range) {
        Type *lo = check_expr(n->lhs);
        Type *hi = check_expr(n->rhs);
        if (!lo || !hi) { pop_scope(); return; }
        if (lo->untyped && !hi->untyped) { apply_type(n->lhs, hi); lo = hi; }
        else if (hi->untyped) { apply_type(n->rhs, lo); }
        lo = settle(n->lhs);
        settle(n->rhs);
        if (!is_integer(lo)) {
          error(n->lhs->pos, "range bounds must be integers, got %s",
                type_str(lo).c_str());
          pop_scope();
          return;
        }
        n->type = lo;
        // The loop variable is a fresh immutable binding each iteration.
        n->sym = declare(n->name, lo, false, n->pos);
      } else if (n->cond) {
        check_condition(n->cond, "for");
      }
      if (n->is_parallel) {
        // Iterations run on different workers, so the body may not carry
        // control out of the loop any more than a task body may.
        open_scopes.push_back(n);
        scope_loop_base.push_back(loop_depth);
        if (!n->text.empty() && !check_reduction(n)) {
          scope_loop_base.pop_back();
          open_scopes.pop_back();
          pop_scope();
          return;
        }
      }
      loop_depth++;
      check_stmt(n->body);
      loop_depth--;
      if (n->is_parallel) {
        check_parallel_body(n);
        scope_loop_base.pop_back();
        open_scopes.pop_back();
      }
      pop_scope();
      break;

    case ND_BREAK:
    case ND_CONTINUE:
      if (loop_depth == 0) {
        error(n->pos, "'%s' outside of a loop",
              n->kind == ND_BREAK ? "break" : "continue");
      } else if (!scope_loop_base.empty() &&
                 loop_depth <= scope_loop_base.back()) {
        leaving_scope(n, n->kind == ND_BREAK ? "break" : "continue");
      }
      break;

    case ND_EXPR_STMT: {
      // Nothing consumes the result here, so a handler block has nothing to
      // produce and may simply fall out.
      if (n->lhs->kind == ND_CATCH || n->lhs->kind == ND_ORELSE)
        n->lhs->form = 1;
      Type *t = check_expr(n->lhs);
      // Discarding a fallible result would be ignoring an error by accident,
      // which the language does not allow.
      if (t && t->is_error_union)
        error(n->lhs->pos,
              "%s cannot be discarded; handle it with 'try' or 'catch'",
              type_str(t).c_str());
      break;
    }

    default:
      error(n->pos, "unexpected statement");
      break;
    }
  }

  // --- declarations ------------------------------------------------------

  // Does this statement definitely leave the function? Used to require a
  // return on every path without building a full control-flow graph.
  bool always_returns(Node *n) {
    if (!n) return false;
    switch (n->kind) {
    case ND_RETURN: return true;
    case ND_BLOCK:
      for (Node *st : n->kids)
        if (always_returns(st)) return true;
      return false;
    case ND_IF:
      return n->els && always_returns(n->body) && always_returns(n->els);
    case ND_FOR:
      return !n->is_range && !n->cond; // `for {}` only exits via return
    default: return false;
    }
  }

  // Like always_returns, but `break` and `continue` count too: they are enough
  // to leave a `catch` block behind.
  bool always_leaves(Node *n) {
    if (!n) return false;
    switch (n->kind) {
    case ND_RETURN: case ND_BREAK: case ND_CONTINUE:
      return true;
    case ND_BLOCK:
      for (Node *st : n->kids)
        if (always_leaves(st)) return true;
      return false;
    case ND_IF:
      return n->els && always_leaves(n->body) && always_leaves(n->els);
    default:
      return always_returns(n);
    }
  }

  void check_func(Node *fn) {
    // The signature pass gives up on a bad declaration without leaving a
    // symbol behind; there is nothing to check here.
    if (fn->is_extern || !fn->sym) return;
    ret_type = fn->sym->type->ret;
    push_scope();
    for (Node *p : fn->kids)
      p->sym = declare(p->name, p->type, p->is_mut, p->pos);
    check_stmt(fn->body);
    pop_scope();

    // A fallible function carrying no value can fall off the end: that path is
    // simply success.
    Type *payload = ret_type->is_error_union ? ret_type->elem : ret_type;
    if (payload->kind != TY_VOID && !always_returns(fn->body))
      error(fn->pos, "'%s' must return %s on every path",
            fn->name.c_str(), type_str(ret_type).c_str());
  }

  // A struct cannot contain itself by value, directly or through another
  // struct: its size would be infinite.
  bool contains_cycle(Type *type, std::vector<Type *> &open) {
    for (Type *seen : open)
      if (seen == type) return true;
    open.push_back(type);
    for (const Field &f : type->fields) {
      Type *t = f.type;
      while (t->kind == TY_ARRAY) t = t->elem;
      if (t->kind == TY_STRUCT && contains_cycle(t, open)) return true;
    }
    open.pop_back();
    return false;
  }

  bool declare_structs() {
    std::vector<Node *> decls;
    std::vector<Node *> ifaces;
    for (Node *decl : pkg.unit->kids) {
      bool is_struct = decl->kind == ND_STRUCT_DECL;
      if (!is_struct && decl->kind != ND_INTERFACE_DECL) continue;
      if (pkg.type_names.count(decl->name) || types.named(decl->name)) {
        error(decl->pos, "'%s' is already a type", decl->name.c_str());
        return false;
      }
      // The registered name is qualified, so two packages may each have a
      // `Node` without colliding in the type table or in the object file.
      if (is_struct) {
        Type *type = types.declare_struct(pkg.prefix + decl->name);
        type->is_extern = decl->is_extern;
        decl->type = type;
        decls.push_back(decl);
      } else {
        decl->type = types.declare_interface(pkg.prefix + decl->name);
        ifaces.push_back(decl);
      }
      pkg.type_names[decl->name] = decl->type;
    }

    for (Node *decl : ifaces) {
      std::unordered_set<std::string> seen;
      for (Node *m : decl->kids) {
        if (!seen.insert(m->name).second) {
          error(m->pos, "duplicate method '%s'", m->name.c_str());
          return false;
        }
        std::vector<Type *> params;
        for (Node *p : m->kids) {
          p->type = resolve(p->type_expr);
          params.push_back(p->type);
        }
        Type *ret = m->type_expr ? resolve(m->type_expr) : types.void_ty;
        decl->type->methods.push_back(
            Field{m->name, types.func(params, ret), 0, 0});
      }
    }

    // Names exist before fields are resolved, so structs can refer to each
    // other through pointers in any order.
    for (Node *decl : decls) {
      std::vector<Field> fields;
      std::unordered_set<std::string> seen;
      for (Node *f : decl->kids) {
        if (!seen.insert(f->name).second) {
          error(f->pos, "duplicate field '%s'", f->name.c_str());
          return false;
        }
        Field field;
        field.name = f->name;
        field.type = resolve(f->type_expr);
        fields.push_back(field);
      }
      types.layout_struct(decl->type, std::move(fields));
    }

    for (Node *decl : decls) {
      std::vector<Type *> open;
      if (contains_cycle(decl->type, open)) {
        error(decl->pos, "struct '%s' contains itself by value",
              decl->name.c_str());
        return false;
      }
    }
    return true;
  }

  bool run() {
    push_scope();
    if (!declare_structs()) return false;

    // Signatures first: within a package, declaration order does not matter.
    for (Node *fn : pkg.unit->kids) {
      if (fn->kind != ND_FUNC) continue;

      // A method becomes an ordinary function whose first parameter is the
      // receiver, named Type.method. Everything downstream then treats it as
      // any other function.
      std::string method_of;
      if (fn->lhs) {
        Type *recv = resolve(fn->lhs->type_expr);
        Type *owner = recv->kind == TY_PTR ? recv->elem : recv;
        if (owner->kind != TY_STRUCT || owner->is_error_union ||
            owner->is_optional) {
          error(fn->lhs->pos, "%s cannot have methods",
                type_str(owner).c_str());
          continue;
        }
        if (fn->is_extern) {
          error(fn->pos, "an extern function cannot have a receiver");
          continue;
        }
        method_of = owner->name;
        fn->kids.insert(fn->kids.begin(), fn->lhs);
      }

      // A generic has no signature until it is instantiated: its parameter
      // types mention type parameters that are not types yet.
      if (!fn->tparams.empty()) {
        if (!method_of.empty()) {
          error(fn->pos, "a method cannot be generic yet");
          continue;
        }
        std::string plain = fn->name;
        Symbol *sym =
            declare(plain, types.func({}, types.void_ty), false, fn->pos);
        sym->is_func = true;
        sym->is_generic = true;
        fn->name = pkg.prefix + plain;
        sym->name = fn->name;
        sym->decl = fn;
        fn->sym = sym;
        pkg.globals[plain] = sym;
        continue;
      }

      std::vector<Type *> params;
      for (Node *p : fn->kids) {
        p->type = resolve(p->type_expr);
        params.push_back(p->type);
      }
      Type *ret = fn->type_expr ? resolve(fn->type_expr) : types.void_ty;

      Symbol *sym;
      if (!method_of.empty()) {
        auto &table = prog.methods[method_of];
        if (table.count(fn->name)) {
          error(fn->pos, "%s already has a method '%s'",
                method_of.c_str(), fn->name.c_str());
          continue;
        }
        std::string plain = fn->name;
        fn->name = method_of + "." + plain;
        sym = ast.make_symbol(fn->name, types.func(params, ret), false,
                              fn->pos);
        table[plain] = sym;
      } else {
        std::string plain = fn->name;
        sym = declare(plain, types.func(params, ret), false, fn->pos);
        // Extern names are C symbols and stay as written; everything else is
        // qualified so two packages can both define `init`.
        if (!fn->is_extern) fn->name = pkg.prefix + plain;
        sym->name = fn->name;
        pkg.globals[plain] = sym;
      }
      sym->is_func = true;
      sym->decl = fn;
      fn->sym = sym;
      if (!method_of.empty()) continue;

      // Aggregates cross our own call boundary as a pointer, which is not the
      // C ABI; the FFI boundary has to stay scalar.
      if (fn->is_extern) {
        for (Type *p : params)
          if (is_aggregate(p))
            error(fn->pos, "extern '%s' cannot take %s; pass .ptr and .len",
                  fn->name.c_str(), type_str(p).c_str());
        if (is_aggregate(ret))
          error(fn->pos, "extern '%s' cannot return %s", fn->name.c_str(),
                type_str(ret).c_str());
      }

      if (fn->name == "main" && pkg.prefix.empty()) {
        Type *payload = ret->is_error_union ? ret->elem : ret;
        if (payload->kind != TY_VOID && payload->kind != TY_INT)
          error(fn->pos, "'main' must return int, void, !int or !void");
        if (!params.empty())
          error(fn->pos, "'main' takes no parameters");
      }
    }

    for (Node *fn : pkg.unit->kids)
      if (fn->kind == ND_FUNC && fn->tparams.empty()) check_func(fn);

    pop_scope();
    return error_count() == 0;
  }

  // An instantiation is checked later, with its type parameters bound and the
  // declaring package's names back in scope.
  bool run_instance(const Program::Instance &job) {
    push_scope();
    for (auto &entry : pkg.globals) scopes.back()[entry.first] = entry.second;

    size_t depth = bindings.size();
    for (auto &entry : job.bind) bindings.push_back(entry);
    check_func(job.decl);
    bindings.resize(depth);

    pop_scope();
    return error_count() == 0;
  }
};

} // namespace

bool check(Program &prog, TypeTable &types) {
  for (Package *pkg : prog.order)
    if (!Checker(prog, *pkg, types).run()) return false;

  Package *main = prog.main();
  if (main && !main->globals.count("main"))
    error(Pos{-1, 0, 0},
          "no 'main' function: a program needs 'func main() int' somewhere in "
          "the package being compiled");

  // Draining the worklist can add to it: an instance body may instantiate
  // something else.
  while (!prog.pending.empty()) {
    Program::Instance job = prog.pending.back();
    prog.pending.pop_back();
    if (!Checker(prog, *job.owner, types).run_instance(job)) return false;
  }
  return error_count() == 0;
}
