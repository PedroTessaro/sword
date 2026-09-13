#include "ast.h"
#include "backend_llvm.h"
#include "check.h"
#include "package.h"
#include "paths.h"
#include "diag.h"
#include "ir.h"
#include "lexer.h"
#include "lower.h"
#include "parser.h"
#include "types.h"

#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <string>
#include <sys/wait.h>
#include <unistd.h>

namespace {

enum Stage { STAGE_TOKENS, STAGE_AST, STAGE_IR, STAGE_LLVM, STAGE_BINARY };

void usage() {
  fputs("usage: shield <file.sw | directory> [options]\n"
        "       shield test <file.sw | directory> [options]\n"
        "\n"
        "  -o <path>      output binary (default: a.out)\n"
        "  -p <n>         test only: how many tests may run at once\n"
        "  -I <dir>       add a directory to the package search path\n"
        "  --mode=<m>     debug | safe | fast | small (default safe)\n"
        "                 debug and safe check bounds and integer overflow\n"
        "  -O<level>      override the optimization level\n"
        "  --emit-tokens  stop after lexing\n"
        "  --emit-ast     stop after parsing and checking\n"
        "  --emit-ir      stop after lowering, print Sword IR\n"
        "  --emit-llvm    stop after codegen, print LLVM IR\n",
        stderr);
}

const char *node_name(NodeKind kind) {
  switch (kind) {
  case ND_UNIT: return "unit";
  case ND_PACKAGE: return "package";
  case ND_IMPORT: return "import";
  case ND_FUNC: return "func";
  case ND_PARAM: return "param";
  case ND_STRUCT_DECL: return "struct";
  case ND_ENUM_DECL: return "enum";
  case ND_ENUM_MEMBER: return "enum-member";
  case ND_FIELD_DECL: return "field-decl";
  case ND_BLOCK: return "block";
  case ND_VAR: return "var";
  case ND_ASSIGN: return "assign";
  case ND_RETURN: return "return";
  case ND_IF: return "if";
  case ND_FOR: return "for";
  case ND_EXPR_STMT: return "expr";
  case ND_BREAK: return "break";
  case ND_CONTINUE: return "continue";
  case ND_INT_LIT: return "int";
  case ND_BOOL_LIT: return "bool";
  case ND_STRING_LIT: return "string";
  case ND_ARRAY_LIT: return "array-lit";
  case ND_STRUCT_LIT: return "struct-lit";
  case ND_FIELD_INIT: return "field-init";
  case ND_IDENT: return "ident";
  case ND_BINARY: return "binary";
  case ND_UNARY: return "unary";
  case ND_CALL: return "call";
  case ND_CONVERT: return "convert";
  case ND_ERROR_LIT: return "error-lit";
  case ND_TRY: return "try";
  case ND_CATCH: return "catch";
  case ND_DEFER: return "defer";
  case ND_SCOPE: return "scope";
  case ND_SPAWN: return "spawn";
  case ND_LOCK: return "lock";
  case ND_SWITCH: return "switch";
  case ND_CASE: return "case";
  case ND_FIELD: return "field";
  case ND_INDEX: return "index";
  case ND_SLICE_EXPR: return "slice";
  default: return "type";
  }
}

void dump_ast(Node *n, int depth) {
  if (!n) return;
  printf("%*s%s", depth * 2, "", node_name(n->kind));
  if (!n->name.empty()) printf(" %s", n->name.c_str());
  if (n->kind == ND_INT_LIT || n->kind == ND_BOOL_LIT)
    printf(" %llu", (unsigned long long)n->ival);
  if (n->kind == ND_STRING_LIT) printf(" \"%s\"", n->text.c_str());
  if (n->is_extern) printf(" extern");
  if (n->op != TK_EOF) printf(" '%s'", tok_name(n->op));
  if (n->is_mut) printf(" mut");
  if (n->type) printf(" : %s", type_str(n->type).c_str());
  putchar('\n');

  dump_ast(n->type_expr, depth + 1);
  for (Node *kid : n->kids) dump_ast(kid, depth + 1);
  dump_ast(n->lhs, depth + 1);
  dump_ast(n->rhs, depth + 1);
  dump_ast(n->cond, depth + 1);
  dump_ast(n->body, depth + 1);
  dump_ast(n->els, depth + 1);
}

// Hands the generated LLVM IR to clang, which assembles and links it. This is
// also where LLVM's own optimization pipeline runs.
bool assemble(const std::string &ll_path, const std::string &out_path,
              const std::string &opt_level, const std::string &runtime,
              const std::string &extra) {
  // The archive only contributes objects the program actually references, so
  // a program that never spawns links nothing from it.
  // `-x none` puts clang back into guess-by-extension mode, so the archive is
  // read as an archive and not as more LLVM IR.
  std::string cmd = "clang -O" + opt_level + " -Wno-override-module -x ir " +
                    ll_path;
  // -pthread because the scheduler runs threads, and on older Linux they are
  // not in libc; on Darwin it is accepted and does nothing.
  if (!runtime.empty()) cmd += " -x none " + runtime + " -lc++ -pthread";
  // Whatever the runtime was built against, from the file beside the archive.
  if (!runtime.empty() && !extra.empty()) cmd += " " + extra;
  cmd += " -o " + out_path;
  int status = system(cmd.c_str());
  if (status != 0) {
    fprintf(stderr, "shield: clang failed while assembling %s\n",
            ll_path.c_str());
    return false;
  }
  return true;
}

} // namespace

int main(int argc, char **argv) {
  const char *input = nullptr;
  std::vector<std::string> search;
  std::string output = "a.out";
  Stage stage = STAGE_BINARY;
  Mode mode = MODE_SAFE;
  std::string opt_level;

  // `shield test <path>` builds the package together with its `*_test.sw`
  // files, behind an entry point that runs them, then runs it.
  bool testing = argc > 1 && !strcmp(argv[1], "test");
  int first = testing ? 2 : 1;
  bool named = false; // whether -o asked for a particular path
  std::string forwarded; // options the test binary reads for itself
  if (testing) output = "";

  for (int i = first; i < argc; i++) {
    const char *arg = argv[i];
    if (!strcmp(arg, "-o") && i + 1 < argc) { output = argv[++i]; named = true; }
    else if (!strcmp(arg, "-I") && i + 1 < argc) search.push_back(argv[++i]);
    else if (!strncmp(arg, "-O", 2) && arg[2]) opt_level = arg + 2;
    else if (!strncmp(arg, "--mode=", 7)) {
      const char *name = arg + 7;
      if (!strcmp(name, "debug")) mode = MODE_DEBUG;
      else if (!strcmp(name, "safe")) mode = MODE_SAFE;
      else if (!strcmp(name, "fast")) mode = MODE_FAST;
      else if (!strcmp(name, "small")) mode = MODE_SMALL;
      else {
        fprintf(stderr, "shield: unknown mode '%s'\n", name);
        return 1;
      }
    }
    else if (!strcmp(arg, "--emit-tokens")) stage = STAGE_TOKENS;
    else if (!strcmp(arg, "--emit-ast")) stage = STAGE_AST;
    else if (!strcmp(arg, "--emit-ir")) stage = STAGE_IR;
    else if (!strcmp(arg, "--emit-llvm")) stage = STAGE_LLVM;
    else if (testing && !strcmp(arg, "-p") && i + 1 < argc)
      forwarded += " -p " + std::string(argv[++i]);
    else if (!strcmp(arg, "-h") || !strcmp(arg, "--help")) { usage(); return 0; }
    else if (arg[0] == '-') { usage(); return 1; }
    else input = arg;
  }

  if (!input) {
    usage();
    return 1;
  }

  // A test binary lands beside the package it tests and is removed after.
  if (testing && output.empty()) output = std::string(input) + ".test";
  if (output.empty()) output = "a.out";

  if (opt_level.empty()) {
    // Debug keeps every local on the stack, which is what makes a debug build
    // readable; the other modes rely on LLVM to promote them.
    opt_level = mode == MODE_DEBUG ? "0" : mode == MODE_SMALL ? "z" : "2";
  }

  if (stage == STAGE_TOKENS) {
    int id = load_source(input);
    if (id < 0) {
      fprintf(stderr, "shield: cannot open '%s'\n", input);
      return 1;
    }
    for (const Token &t : lex(id))
      printf("%4d:%-3d %s%s%s\n", t.pos.line, t.pos.col, tok_name(t.kind),
             t.text.empty() ? "" : " ", t.text.c_str());
    return error_count() > 0 ? 1 : 0;
  }

  // The program's own directory comes first so it can sit next to the packages
  // it imports; the rest is wherever the compiler keeps its standard library.
  search.push_back(directory_of(input));
  for (const std::string &root : package_roots()) search.push_back(root);

  Program prog;
  if (!load_program(input, search, prog,
                    testing ? LOAD_TESTS : LOAD_BUILD))
    return 1;

  TypeTable types;
  if (!check(prog, types)) return 1;

  if (stage == STAGE_AST) {
    for (Package *pkg : prog.order) {
      printf("// package %s\n",
             pkg->import_path.empty() ? "main" : pkg->import_path.c_str());
      dump_ast(pkg->unit, 0);
    }
    return 0;
  }

  IrModule mod;
  lower(prog, types, mode, mod);

  if (stage == STAGE_IR) {
    ir_print(mod, stdout);
    return 0;
  }

  if (stage == STAGE_LLVM) {
    emit_llvm(mod, stdout);
    return 0;
  }

  std::string ll_path = output + ".ll";
  FILE *ll = fopen(ll_path.c_str(), "w");
  if (!ll) {
    fprintf(stderr, "shield: cannot write '%s'\n", ll_path.c_str());
    return 1;
  }
  emit_llvm(mod, ll);
  fclose(ll);

  bool ok = assemble(ll_path, output, opt_level, runtime_archive(),
                     runtime_link_flags());
  unlink(ll_path.c_str());
  if (!ok) return 1;
  if (!testing) return 0;

  // Run it, hand back what it says, and leave nothing behind.
  std::string command = output;
  if (command.find('/') == std::string::npos) command = "./" + command;
  command += forwarded;
  int status = system(command.c_str());
  // A name the user asked for is theirs to keep.
  if (!named) unlink(output.c_str());
  if (status == -1) return 1;
  return WIFEXITED(status) ? WEXITSTATUS(status) : 1;
}
