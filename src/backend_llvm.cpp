#include "backend_llvm.h"

#include <cstring>
#include <set>
#include <string>
#include <vector>

namespace {

std::string ll_type(const Type *t) {
  switch (t->kind) {
  case TY_VOID: return "void";
  case TY_BOOL: return "i1";
  case TY_INT: return "i" + std::to_string(t->bits);
  case TY_FLOAT: return t->bits == 32 ? "float" : "double";
  case TY_PTR: case TY_RAWPTR: case TY_FUNC: return "ptr";
  case TY_OPT: case TY_ATOMIC: return ll_type(t->elem);
  case TY_SLICE: case TY_STRING: return "%slice";
  case TY_ARRAY:
    return "[" + std::to_string(t->count) + " x " + ll_type(t->elem) + "]";
  case TY_STRUCT: return "%" + t->name;
  }
  return "void";
}

std::string quote(const std::string &bytes) {
  static const char *hex = "0123456789ABCDEF";
  std::string out;
  for (unsigned char c : bytes) {
    if (c == '"' || c == '\\' || c < 0x20 || c >= 0x7f) {
      out += '\\';
      out += hex[c >> 4];
      out += hex[c & 15];
    } else {
      out += (char)c;
    }
  }
  return out;
}

// Sword's entry point returns `int`, but the process exit status is 32 bits,
// so the real @main is a wrapper around it.
const char *entry_name(const std::string &name) {
  return name == "main" ? "sword_main" : nullptr;
}

struct Emitter {
  const IrModule &mod;
  FILE *out;
  const IrFunc *fn = nullptr;
  std::vector<std::string> operand; // value id -> LLVM operand text
  std::set<std::string> intrinsics;
  std::set<std::string> declared; // names the source already declares extern

  Emitter(const IrModule &m, FILE *o) : mod(m), out(o) {}

  const std::string &val(int id) const { return operand[id]; }
  std::string type_of(int id) const { return ll_type(fn->value_type[id]); }
  std::string typed(int id) const { return type_of(id) + " " + val(id); }

  static int align_for(const Type *t) { return (int)align_of(t); }

  const char *arith(const IrInst &in) {
    bool sign = in.type->is_signed;
    if (in.type->kind == TY_FLOAT) {
      switch (in.op) {
      case IR_ADD: return "fadd";
      case IR_SUB: return "fsub";
      case IR_MUL: return "fmul";
      case IR_DIV: return "fdiv";
      case IR_MOD: return "frem";
      default: return nullptr;
      }
    }
    switch (in.op) {
    case IR_ADD: return "add";
    case IR_SUB: return "sub";
    case IR_MUL: return "mul";
    case IR_DIV: return sign ? "sdiv" : "udiv";
    case IR_MOD: return sign ? "srem" : "urem";
    case IR_AND: return "and";
    case IR_OR: return "or";
    case IR_XOR: return "xor";
    case IR_SHL: return "shl";
    case IR_SHR: return sign ? "ashr" : "lshr";
    default: return nullptr;
    }
  }

  const char *predicate(const IrInst &in) {
    const Type *operand = fn->value_type[in.a];
    if (operand->kind == TY_FLOAT) {
      switch (in.op) {
      case IR_EQ: return "oeq";
      case IR_NE: return "one";
      case IR_LT: return "olt";
      case IR_LE: return "ole";
      case IR_GT: return "ogt";
      case IR_GE: return "oge";
      default: return nullptr;
      }
    }
    bool sign = operand->is_signed;
    switch (in.op) {
    case IR_EQ: return "eq";
    case IR_NE: return "ne";
    case IR_LT: return sign ? "slt" : "ult";
    case IR_LE: return sign ? "sle" : "ule";
    case IR_GT: return sign ? "sgt" : "ugt";
    case IR_GE: return sign ? "sge" : "uge";
    default: return nullptr;
    }
  }

  std::string overflow_intrinsic(const IrInst &in) {
    const char *name = in.imm == IR_ADD ? "add"
                       : in.imm == IR_SUB ? "sub"
                                          : "mul";
    return std::string("llvm.") + (in.type->is_signed ? "s" : "u") + name +
           ".with.overflow." + ll_type(in.type);
  }

  void inst(const IrInst &in) {
    switch (in.op) {
    case IR_CONST:
      return; // folded into the operand table, no instruction needed

    case IR_STR_VALUE:
    case IR_STR_DATA:
    case IR_VTABLE:
    case IR_FUNCADDR:
      return; // globals are referenced by name

    case IR_ALLOCA:
      fprintf(out, "  %s = alloca %s, align %d\n", val(in.dst).c_str(),
              ll_type(in.type).c_str(), align_for(in.type));
      break;

    case IR_LOAD:
      fprintf(out, "  %s = load %s, ptr %s, align %d\n", val(in.dst).c_str(),
              ll_type(in.type).c_str(), val(in.a).c_str(), align_for(in.type));
      break;

    case IR_STORE:
      fprintf(out, "  store %s %s, ptr %s, align %d\n",
              ll_type(in.type).c_str(), val(in.a).c_str(), val(in.b).c_str(),
              align_for(in.type));
      break;

    case IR_MEMCPY:
      fprintf(out,
              "  call void @llvm.memcpy.p0.p0.i64(ptr %s, ptr %s, i64 %lld, "
              "i1 false)\n",
              val(in.a).c_str(), val(in.b).c_str(), (long long)in.imm);
      break;

    case IR_ZERO:
      fprintf(out,
              "  call void @llvm.memset.p0.i64(ptr %s, i8 0, i64 %lld, "
              "i1 false)\n",
              val(in.a).c_str(), (long long)in.imm);
      break;

    case IR_GEP_FIELD:
      fprintf(out, "  %s = getelementptr inbounds %s, ptr %s, i32 0, i32 %lld\n",
              val(in.dst).c_str(), ll_type(in.type).c_str(), val(in.a).c_str(),
              (long long)in.imm);
      break;

    case IR_GEP_INDEX:
      fprintf(out, "  %s = getelementptr inbounds %s, ptr %s, i64 %s\n",
              val(in.dst).c_str(), ll_type(in.type).c_str(), val(in.a).c_str(),
              val(in.b).c_str());
      break;

    case IR_CAST: {
      const Type *from = fn->value_type[in.a];
      const Type *to = in.type;
      auto is_address = [](const Type *x) {
        return x->kind == TY_PTR || x->kind == TY_RAWPTR ||
               x->kind == TY_FUNC || x->kind == TY_OPT;
      };
      const char *how;
      if (is_address(from) && to->kind == TY_INT)
        how = "ptrtoint";
      else if (from->kind == TY_INT && is_address(to))
        how = "inttoptr";
      else if (from->kind == TY_FLOAT && to->kind == TY_FLOAT)
        how = from->bits > to->bits ? "fptrunc" : "fpext";
      else if (from->kind == TY_FLOAT)
        how = to->is_signed ? "fptosi" : "fptoui";
      else if (to->kind == TY_FLOAT)
        how = from->is_signed ? "sitofp" : "uitofp";
      else
        how = from->bits > to->bits ? "trunc"
              : from->is_signed     ? "sext"
                                    : "zext";
      fprintf(out, "  %s = %s %s to %s\n", val(in.dst).c_str(), how,
              typed(in.a).c_str(), ll_type(to).c_str());
      break;
    }

    case IR_OVF: {
      std::string name = overflow_intrinsic(in);
      std::string ret = "{ " + ll_type(in.type) + ", i1 }";
      intrinsics.insert("declare " + ret + " @" + name + "(" +
                        ll_type(in.type) + ", " + ll_type(in.type) + ")");
      fprintf(out, "  %%t%d = call %s @%s(%s, %s)\n", in.dst, ret.c_str(),
              name.c_str(), typed(in.a).c_str(), typed(in.b).c_str());
      fprintf(out, "  %s = extractvalue %s %%t%d, 1\n", val(in.dst).c_str(),
              ret.c_str(), in.dst);
      break;
    }

    case IR_ATOMIC_RMW: {
      static const char *names[] = {"add", "sub", "and", "or",  "xor",
                                    "min", "max", "xchg", "fadd"};
      const char *name = names[in.imm];
      // Unsigned minimum and maximum are spelled differently.
      if ((in.imm == RMW_MIN || in.imm == RMW_MAX) && !in.type->is_signed)
        name = in.imm == RMW_MIN ? "umin" : "umax";
      if (in.type->kind == TY_BOOL) {
        fprintf(out, "  %%wide%d = zext i1 %s to i8\n", in.dst,
                val(in.b).c_str());
        fprintf(out, "  %%raw%d = atomicrmw %s ptr %s, i8 %%wide%d seq_cst\n",
                in.dst, name, val(in.a).c_str(), in.dst);
        fprintf(out, "  %s = trunc i8 %%raw%d to i1\n", val(in.dst).c_str(),
                in.dst);
        break;
      }
      if (in.dst >= 0)
        fprintf(out, "  %s = atomicrmw %s ptr %s, %s %s seq_cst\n",
                val(in.dst).c_str(), name, val(in.a).c_str(),
                ll_type(in.type).c_str(), val(in.b).c_str());
      else
        fprintf(out, "  %%drop%d = atomicrmw %s ptr %s, %s %s seq_cst\n", in.a,
                name, val(in.a).c_str(), ll_type(in.type).c_str(),
                val(in.b).c_str());
      break;
    }

    case IR_ATOMIC_LOAD:
      // LLVM will not touch an i1 atomically, so a bool travels as a byte.
      if (in.type->kind == TY_BOOL) {
        fprintf(out, "  %%wide%d = load atomic i8, ptr %s seq_cst, align 1\n",
                in.dst, val(in.a).c_str());
        fprintf(out, "  %s = trunc i8 %%wide%d to i1\n", val(in.dst).c_str(),
                in.dst);
      } else {
        fprintf(out, "  %s = load atomic %s, ptr %s seq_cst, align %d\n",
                val(in.dst).c_str(), ll_type(in.type).c_str(),
                val(in.a).c_str(), align_for(in.type));
      }
      break;

    case IR_ATOMIC_STORE:
      if (in.type->kind == TY_BOOL) {
        fprintf(out, "  %%wide%d = zext i1 %s to i8\n", in.a,
                val(in.a).c_str());
        fprintf(out, "  store atomic i8 %%wide%d, ptr %s seq_cst, align 1\n",
                in.a, val(in.b).c_str());
      } else {
        fprintf(out, "  store atomic %s %s, ptr %s seq_cst, align %d\n",
                ll_type(in.type).c_str(), val(in.a).c_str(),
                val(in.b).c_str(), align_for(in.type));
      }
      break;

    case IR_ATOMIC_CAS: {
      if (in.type->kind == TY_BOOL) {
        fprintf(out, "  %%exp%d = zext i1 %s to i8\n", in.dst,
                val(in.b).c_str());
        fprintf(out, "  %%des%d = zext i1 %s to i8\n", in.dst,
                val(in.args[0]).c_str());
        fprintf(out,
                "  %%cas%d = cmpxchg ptr %s, i8 %%exp%d, i8 %%des%d seq_cst "
                "seq_cst\n",
                in.dst, val(in.a).c_str(), in.dst, in.dst);
        fprintf(out, "  %s = extractvalue { i8, i1 } %%cas%d, 1\n",
                val(in.dst).c_str(), in.dst);
        break;
      }
      std::string pair = "{ " + ll_type(in.type) + ", i1 }";
      fprintf(out, "  %%cas%d = cmpxchg ptr %s, %s %s, %s %s seq_cst seq_cst\n",
              in.dst, val(in.a).c_str(), ll_type(in.type).c_str(),
              val(in.b).c_str(), ll_type(in.type).c_str(),
              val(in.args[0]).c_str());
      fprintf(out, "  %s = extractvalue %s %%cas%d, 1\n", val(in.dst).c_str(),
              pair.c_str(), in.dst);
      break;
    }

    case IR_NEG:
      if (in.type->kind == TY_FLOAT)
        fprintf(out, "  %s = fneg %s %s\n", val(in.dst).c_str(),
                ll_type(in.type).c_str(), val(in.a).c_str());
      else
        fprintf(out, "  %s = sub %s 0, %s\n", val(in.dst).c_str(),
                ll_type(in.type).c_str(), val(in.a).c_str());
      break;

    case IR_NOT:
      fprintf(out, "  %s = xor i1 %s, true\n", val(in.dst).c_str(),
              val(in.a).c_str());
      break;

    case IR_EQ: case IR_NE: case IR_LT: case IR_LE: case IR_GT: case IR_GE:
      fprintf(out, "  %s = %s %s %s %s, %s\n", val(in.dst).c_str(),
              fn->value_type[in.a]->kind == TY_FLOAT ? "fcmp" : "icmp",
              predicate(in), type_of(in.a).c_str(), val(in.a).c_str(),
              val(in.b).c_str());
      break;

    case IR_CALL: {
      std::string name = in.callee;
      if (const char *renamed = entry_name(name)) name = renamed;
      fputs("  ", out);
      if (in.dst >= 0) fprintf(out, "%s = ", val(in.dst).c_str());
      // An empty callee means the target came out of a vtable.
      if (name.empty())
        fprintf(out, "call %s %s(", ll_type(in.type).c_str(),
                val(in.a).c_str());
      else
        fprintf(out, "call %s @%s(", ll_type(in.type).c_str(), name.c_str());
      for (size_t i = 0; i < in.args.size(); i++)
        fprintf(out, "%s%s", i ? ", " : "", typed(in.args[i]).c_str());
      fputs(")\n", out);
      break;
    }

    case IR_BR:
      fprintf(out, "  br label %%bb%d\n", in.target);
      break;

    case IR_CONDBR:
      fprintf(out, "  br i1 %s, label %%bb%d, label %%bb%d\n",
              val(in.a).c_str(), in.target, in.target2);
      break;

    case IR_RET:
      if (in.a >= 0) fprintf(out, "  ret %s\n", typed(in.a).c_str());
      else fputs("  ret void\n", out);
      break;

    case IR_UNREACHABLE:
      fputs("  unreachable\n", out);
      break;

    default:
      fprintf(out, "  %s = %s %s %s, %s\n", val(in.dst).c_str(), arith(in),
              ll_type(in.type).c_str(), val(in.a).c_str(), val(in.b).c_str());
      break;
    }
  }

  void signature(const IrFunc &f, const char *keyword) {
    const char *renamed = entry_name(f.name);
    const char *name = renamed ? renamed : f.name.c_str();
    std::string ret = f.ret_by_pointer ? "void" : ll_type(f.ret);

    fprintf(out, "%s %s @%s(", keyword, ret.c_str(), name);
    int index = 0;
    bool named = f.blocks.size() > 0;
    if (f.ret_by_pointer) {
      fputs(named ? "ptr %v0" : "ptr", out);
      index = 1;
    }
    for (size_t i = 0; i < f.params.size(); i++) {
      if (index) fputs(", ", out);
      std::string type =
          is_aggregate(f.params[i]) ? "ptr" : ll_type(f.params[i]);
      if (named) fprintf(out, "%s %%v%d", type.c_str(), index);
      else fputs(type.c_str(), out);
      index++;
    }
    fputc(')', out);
  }

  void func(const IrFunc &f) {
    if (f.is_extern) {
      if (!declared.insert(f.name).second) return;
      signature(f, "declare");
      fputc('\n', out);
      return;
    }

    fn = &f;
    operand.assign(f.value_type.size(), "");
    for (size_t i = 0; i < operand.size(); i++)
      operand[i] = "%v" + std::to_string(i);

    // Constants and globals have no instruction in LLVM; they are spelled
    // inline wherever they are used.
    for (const IrBlock &bb : f.blocks) {
      for (const IrInst &in : bb.insts) {
        if (in.op == IR_CONST) {
          if (in.type->kind == TY_FLOAT) {
            double value = in.type->bits == 32 ? (double)(float)in.fimm
                                               : in.fimm;
            uint64_t bits;
            memcpy(&bits, &value, sizeof(bits));
            char buf[32];
            snprintf(buf, sizeof(buf), "0x%016llX",
                     (unsigned long long)bits);
            operand[in.dst] = buf;
          } else {
            operand[in.dst] = ll_type(in.type) == "ptr" && in.imm == 0
                                  ? "null"
                                  : std::to_string(in.imm);
          }
        }
        else if (in.op == IR_STR_VALUE)
          operand[in.dst] = "@strv." + std::to_string(in.imm);
        else if (in.op == IR_STR_DATA)
          operand[in.dst] = "@str." + std::to_string(in.imm);
        else if (in.op == IR_VTABLE)
          operand[in.dst] = "@vt." + std::to_string(in.imm);
        else if (in.op == IR_FUNCADDR)
          operand[in.dst] = "@" + in.callee;
      }
    }

    signature(f, "define");
    fputs(" {\n", out);
    for (const IrBlock &bb : f.blocks) {
      fprintf(out, "bb%d:\n", bb.id);
      for (const IrInst &in : bb.insts) inst(in);
    }
    fputs("}\n\n", out);
  }

  // A program only gets the argument vector if it asks: `main` takes no
  // parameters otherwise, and nothing from the runtime gets linked in.
  bool wants_args() const {
    return calls("sword_os_argc") || calls("sword_os_arg");
  }

  void entry_wrapper(const IrFunc &f) {
    if (wants_args()) {
      fputs("define i32 @main(i32 %argc, ptr %argv) {\nbb0:\n"
            "  call void @sword_os_set_args(i32 %argc, ptr %argv)\n",
            out);
    } else {
      fputs("define i32 @main() {\nbb0:\n", out);
    }
    if (f.ret->is_error_union) {
      // A fallible main reports the error code as its exit status, and its
      // ordinary result when it succeeds.
      const Type *payload = f.ret->elem;
      const Field *code = find_field(f.ret, "code");
      std::string box = ll_type(f.ret);
      fprintf(out, "  %%box = alloca %s, align %d\n", box.c_str(),
              (int)align_of(f.ret));
      fputs("  call void @sword_main(ptr %box)\n", out);
      fprintf(out,
              "  %%at = getelementptr inbounds %s, ptr %%box, i32 0, i32 %d\n",
              box.c_str(), code->index);
      fputs("  %code = load i16, ptr %at, align 2\n"
            "  %failed = icmp ne i16 %code, 0\n"
            "  br i1 %failed, label %bad, label %good\n"
            "bad:\n"
            "  %status = zext i16 %code to i32\n"
            "  ret i32 %status\n"
            "good:\n",
            out);
      if (payload->kind == TY_VOID) {
        fputs("  ret i32 0\n", out);
      } else {
        const Field *value = find_field(f.ret, "value");
        fprintf(out,
                "  %%vat = getelementptr inbounds %s, ptr %%box, i32 0, i32 %d\n",
                box.c_str(), value->index);
        fprintf(out, "  %%r = load %s, ptr %%vat, align %d\n",
                ll_type(payload).c_str(), (int)align_of(payload));
        if (payload->bits == 32)
          fputs("  ret i32 %r\n", out);
        else
          fprintf(out, "  %%res = trunc %s %%r to i32\n  ret i32 %%res\n",
                  ll_type(payload).c_str());
      }
    } else if (f.ret->kind == TY_VOID) {
      fputs("  call void @sword_main()\n  ret i32 0\n", out);
    } else {
      fprintf(out, "  %%r = call %s @sword_main()\n", ll_type(f.ret).c_str());
      if (f.ret->bits == 32) {
        fputs("  ret i32 %r\n", out);
      } else {
        fprintf(out, "  %%code = trunc %s %%r to i32\n  ret i32 %%code\n",
                ll_type(f.ret).c_str());
      }
    }
    fputs("}\n\n", out);
  }

  // A failed safety check writes its message to stderr and aborts. It is only
  // emitted when something can reach it.
  void panic_helper() {
    if (!declared.count("write"))
      fputs("declare i64 @write(i32, ptr, i64)\n", out);
    if (!declared.count("abort"))
      fputs("declare void @abort()\n", out);
    fputs("\ndefine internal void @sword_panic(ptr %msg, i64 %len) noreturn {\n"
          "bb0:\n"
          "  %w = call i64 @write(i32 2, ptr %msg, i64 %len)\n"
          "  call void @abort()\n"
          "  unreachable\n"
          "}\n\n",
          out);
  }

  // The scheduler lives in libsword_rt.a; generated code only needs to know
  // the shapes it calls.
  void runtime_declarations() {
    static const struct {
      const char *name;
      const char *decl;
    } runtime[] = {
        {"sword_scope_begin", "declare void @sword_scope_begin(ptr)"},
        {"sword_scope_spawn",
         "declare void @sword_scope_spawn(ptr, ptr, ptr, i64)"},
        {"sword_scope_end", "declare i16 @sword_scope_end(ptr)"},
        {"sword_parallel_for",
         "declare i16 @sword_parallel_for(i64, i64, ptr, ptr)"},
        {"sword_mutex_lock", "declare void @sword_mutex_lock(ptr)"},
        {"sword_mutex_unlock", "declare void @sword_mutex_unlock(ptr)"},
    };
    bool any = false;
    for (const auto &entry : runtime) {
      if (!calls(entry.name)) continue;
      fprintf(out, "%s\n", entry.decl);
      any = true;
    }
    // This one is called by the entry wrapper rather than by lowered code.
    if (wants_args()) {
      fputs("declare void @sword_os_set_args(i32, ptr)\n", out);
      any = true;
    }
    if (any) fputc('\n', out);
  }

  bool calls(const std::string &name) const {
    for (const IrFunc &f : mod.funcs)
      for (const IrBlock &bb : f.blocks)
        for (const IrInst &in : bb.insts)
          if (in.op == IR_CALL && in.callee == name) return true;
    return false;
  }

  bool uses_panic() const {
    for (const IrFunc &f : mod.funcs)
      for (const IrBlock &bb : f.blocks)
        for (const IrInst &in : bb.insts)
          if (in.op == IR_CALL && in.callee == "sword_panic") return true;
    return false;
  }

  void run() {
    fputs("; generated by shield\n\n", out);

    // Both are part of the language rather than of any program, and `any`
    // refers to `slice`, so neither is worth making conditional.
    fputs("%slice = type { ptr, i64 }\n", out);

    for (Type *s : mod.structs) {
      fprintf(out, "%%%s = type { ", s->name.c_str());
      for (size_t i = 0; i < s->fields.size(); i++)
        fprintf(out, "%s%s", i ? ", " : "", ll_type(s->fields[i].type).c_str());
      fputs(" }\n", out);
    }
    fputc('\n', out);

    for (size_t i = 0; i < mod.strings.size(); i++) {
      const std::string &text = mod.strings[i];
      fprintf(out, "@str.%zu = private unnamed_addr constant [%zu x i8] c\"%s\"\n",
              i, text.size(), quote(text).c_str());
      fprintf(out,
              "@strv.%zu = private unnamed_addr constant %%slice "
              "{ ptr @str.%zu, i64 %zu }\n",
              i, i, text.size());
    }
    if (!mod.strings.empty()) fputc('\n', out);

    for (size_t i = 0; i < mod.vtables.size(); i++) {
      const auto &entries = mod.vtables[i];
      fprintf(out, "@vt.%zu = private unnamed_addr constant [%zu x ptr] [", i,
              entries.size());
      for (size_t j = 0; j < entries.size(); j++)
        fprintf(out, "%sptr @%s", j ? ", " : "", entries[j].c_str());
      fputs("]\n", out);
    }
    if (!mod.vtables.empty()) fputc('\n', out);

    // Extern declarations first: the panic helper needs to know whether the
    // source already declared write or abort.
    for (const IrFunc &f : mod.funcs)
      if (f.is_extern) func(f);
    if (!mod.funcs.empty()) fputc('\n', out);

    bool panics = uses_panic();
    if (panics) panic_helper();
    runtime_declarations();

    const IrFunc *entry = nullptr;
    for (const IrFunc &f : mod.funcs) {
      if (f.is_extern) continue;
      func(f);
      if (f.name == "main") entry = &f;
    }
    if (entry) entry_wrapper(*entry);

    bool uses_memcpy = false;
    bool uses_memset = false;
    for (const IrFunc &f : mod.funcs)
      for (const IrBlock &bb : f.blocks)
        for (const IrInst &in : bb.insts) {
          if (in.op == IR_MEMCPY) uses_memcpy = true;
          if (in.op == IR_ZERO) uses_memset = true;
        }
    if (uses_memcpy)
      fputs("declare void @llvm.memcpy.p0.p0.i64(ptr, ptr, i64, i1)\n", out);
    if (uses_memset)
      fputs("declare void @llvm.memset.p0.i64(ptr, i8, i64, i1)\n", out);
    for (const std::string &decl : intrinsics) fprintf(out, "%s\n", decl.c_str());
  }
};

} // namespace

void emit_llvm(const IrModule &mod, FILE *out) { Emitter(mod, out).run(); }
