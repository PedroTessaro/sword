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
#include "version.h"
#include "types.h"

#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <poll.h>
#include <signal.h>
#include <string>
#include <sys/wait.h>
#include <time.h>
#include <unistd.h>
#include <vector>

namespace {

enum Stage { STAGE_TOKENS, STAGE_AST, STAGE_IR, STAGE_LLVM, STAGE_BINARY };

void usage() {
  fputs("usage: shield <file.sword | directory> [options]\n"
        "       shield test <file.sword | directory> [options]\n"
        "\n"
        "  -o <path>      output binary (default: a.out)\n"
        "  -p <n>         test only: how many tests may run at once\n"
        "  -run <name>    test only: run just this one; repeat for more\n"
        "  -bench         test only: run the Benchmark... functions instead\n"
        "  -sim           test only: run each test under simulated scheduling,\n"
        "                 one thread and a virtual clock, once per seed\n"
        "  -seeds <n>     test only, with -sim: how many seeds (default 100)\n"
        "  -seed <n>      test only, with -sim: just this seed\n"
        "  --sim          also switch tasks at atomics when SWORD_SIM_SEED runs it\n"
        "  -I <dir>       add a directory to the package search path\n"
        "  --link <arg>   an object, a library or a linker option to link\n"
        "                 against; one argument each, repeat for more\n"
        "  --mode=<m>     debug | safe | fast | small (default safe)\n"
        "                 debug and safe check bounds and integer overflow\n"
        "  -O<level>      override the optimization level\n"
        "  --emit-tokens  stop after lexing\n"
        "  --emit-ast     stop after parsing and checking\n"
        "  --emit-ir      stop after lowering, print Sword IR\n"
        "  --emit-llvm    stop after codegen, print LLVM IR\n"
        "  --version      print the version and stop\n",
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

// The command goes to a shell, so anything that came from the command line is
// quoted on its way in: a path with a space in it is ordinary, and a path with
// anything worse in it should reach the linker as a path.
std::string quoted(const std::string &arg) {
  std::string out = "'";
  for (char c : arg) {
    if (c == '\'') out += "'\\''";
    else out += c;
  }
  return out + "'";
}

// Hands the generated LLVM IR to clang, which assembles and links it. This is
// also where LLVM's own optimization pipeline runs.
bool assemble(const std::string &ll_path, const std::string &out_path,
              const std::string &opt_level, const std::string &runtime,
              const std::string &extra, const std::vector<std::string> &link) {
  // The archive only contributes objects the program actually references, so
  // a program that never spawns links nothing from it.
  // `-x none` puts clang back into guess-by-extension mode, so the archive is
  // read as an archive and not as more LLVM IR.
  std::string cmd = "clang -O" + opt_level + " -Wno-override-module -x ir " +
                    quoted(ll_path);
  // -pthread because the scheduler runs threads, and on older Linux they are
  // not in libc; on Darwin it is accepted and does nothing.
  if (!runtime.empty()) cmd += " -x none " + runtime + " -pthread";
  // Whatever the runtime was built against, from the file beside the archive:
  // OpenSSL when it has TLS, and always the C++ library of the compiler that
  // built it.
  if (!runtime.empty() && !extra.empty()) cmd += " " + extra;
  // After the program's own object, which is where a linker expects to be told
  // what resolves what is still missing.
  for (const std::string &arg : link) cmd += " " + quoted(arg);
  cmd += " -o " + quoted(out_path);
  int status = system(cmd.c_str());
  if (status != 0) {
    fprintf(stderr, "shield: clang failed while assembling %s\n",
            ll_path.c_str());
    return false;
  }
  return true;
}

// --- shield test -sim --------------------------------------------------------

// A call out of the language does whatever it does, on the machine's time, and
// the simulator can neither schedule around it nor play it back. The standard
// library's own calls out are the runtime's, which the simulator knows; any
// other package's are worth saying out loud, once each.
void warn_calls_out(Node *n) {
  if (!n) return;
  if (n->kind == ND_CALL && n->sym && n->sym->decl && n->sym->decl->is_extern)
    warning(n->pos,
            "this calls out of the language (%s); the simulator cannot "
            "replay what it does",
            n->sym->name.c_str());
  for (Node *kid : n->kids) warn_calls_out(kid);
  warn_calls_out(n->lhs);
  warn_calls_out(n->rhs);
  warn_calls_out(n->cond);
  warn_calls_out(n->body);
  warn_calls_out(n->els);
}

void warn_unsimulated(Program &prog) {
  for (Package *pkg : prog.order) {
    if (pkg->import_path.compare(0, 4, "std/") == 0) continue;
    for (Node *decl : pkg->unit->kids)
      if (decl->kind == ND_FUNC) warn_calls_out(decl->body);
  }
}

// A run of the test binary, and everything it said.
struct Captured {
  int status = -1;    // exit status, or -1 when it did not exit on its own
  bool timed_out = false;
  std::string out;
};

// Runs `binary args...` with SWORD_SIM_SEED set, collecting stdout and stderr
// together, and gives up after `limit` seconds: a task that never waits cannot
// be switched away from, and then only the clock outside says so.
Captured run_captured(const std::string &binary,
                      const std::vector<std::string> &args, const char *seed,
                      int limit) {
  Captured got;
  int pipe_ends[2];
  if (pipe(pipe_ends) != 0) return got;
  pid_t child = fork();
  if (child < 0) return got;
  if (child == 0) {
    if (seed) setenv("SWORD_SIM_SEED", seed, 1);
    else unsetenv("SWORD_SIM_SEED");
    dup2(pipe_ends[1], 1);
    dup2(pipe_ends[1], 2);
    close(pipe_ends[0]);
    close(pipe_ends[1]);
    std::vector<char *> argv{const_cast<char *>(binary.c_str())};
    for (const std::string &a : args) argv.push_back(const_cast<char *>(a.c_str()));
    argv.push_back(nullptr);
    execv(binary.c_str(), argv.data());
    _exit(127);
  }
  close(pipe_ends[1]);
  time_t until = time(nullptr) + limit;
  char buffer[4096];
  while (true) {
    pollfd watch{pipe_ends[0], POLLIN, 0};
    int ready = poll(&watch, 1, 200);
    if (ready > 0) {
      ssize_t n = read(pipe_ends[0], buffer, sizeof(buffer));
      if (n <= 0) break;
      got.out.append(buffer, (size_t)n);
      continue;
    }
    if (time(nullptr) >= until) {
      kill(child, SIGKILL);
      got.timed_out = true;
      break;
    }
  }
  close(pipe_ends[0]);
  int status = 0;
  waitpid(child, &status, 0);
  if (!got.timed_out && WIFEXITED(status)) got.status = WEXITSTATUS(status);
  return got;
}

void indented(const std::string &text) {
  size_t at = 0;
  while (at < text.size()) {
    size_t end = text.find('\n', at);
    if (end == std::string::npos) end = text.size();
    printf("     %s\n", text.substr(at, end - at).c_str());
    at = end + 1;
  }
}

// Each test on its own, once per seed: a failure comes with the seed that
// makes it happen again. The runtime's answers are read from its exit status —
// 3 is a deadlock it found, 4 something it does not simulate yet.
int simulate(const std::string &output, const char *input, uint64_t first,
             uint64_t count, bool one_seed) {
  std::string binary = output;
  if (binary.find('/') == std::string::npos) binary = "./" + binary;
  Captured names = run_captured(binary, {"-list"}, nullptr, 60);
  if (names.status != 0) {
    fputs(names.out.c_str(), stdout);
    return 1;
  }
  std::vector<std::string> tests;
  size_t at = 0;
  while (at < names.out.size()) {
    size_t end = names.out.find('\n', at);
    if (end == std::string::npos) end = names.out.size();
    if (end > at) tests.push_back(names.out.substr(at, end - at));
    at = end + 1;
  }

  int failed = 0, skipped = 0;
  for (const std::string &test : tests) {
    bool bad = false;
    for (uint64_t i = 0; i < count && !bad; i++) {
      uint64_t s = one_seed ? first : i + 1;
      std::string seed_text = std::to_string(s);
      Captured run = run_captured(binary, {"-only", test}, seed_text.c_str(), 60);
      if (run.status == 0) {
        if (one_seed) fputs(run.out.c_str(), stdout);
        continue;
      }
      if (run.status == 4) {
        printf("SKIP  %s: %s", test.c_str(),
               run.out.find("network") != std::string::npos
                   ? "uses the network, which -sim does not simulate yet\n"
                   : "waits on a descriptor, which -sim does not simulate yet\n");
        skipped++;
        bad = true;
        break;
      }
      failed++;
      bad = true;
      if (run.timed_out)
        printf("FAIL  %s under seed %llu: did not finish in 60 s — a task that "
               "never waits cannot be switched away from\n",
               test.c_str(), (unsigned long long)s);
      else if (run.status == 3)
        printf("FAIL  %s under seed %llu: deadlock\n", test.c_str(),
               (unsigned long long)s);
      else
        printf("FAIL  %s under seed %llu\n", test.c_str(), (unsigned long long)s);
      indented(run.out);
      printf("      reproduce: shield test -sim -seed %llu -run %s %s\n",
             (unsigned long long)s, test.c_str(), input);
    }
  }
  size_t ran = tests.size() - (size_t)skipped;
  printf("%s  %zu tests", failed ? "FAIL" : "ok  ", ran);
  if (failed) printf(", %d failed", failed);
  if (skipped) printf(", %d skipped", skipped);
  if (one_seed)
    printf(", seed %llu\n", (unsigned long long)first);
  else
    printf(", %llu seeds each\n", (unsigned long long)count);
  return failed ? 1 : 0;
}

} // namespace

int main(int argc, char **argv) {
  const char *input = nullptr;
  std::vector<std::string> search;
  // Objects and libraries of the caller's own, in the order they were given.
  std::vector<std::string> link_with;
  std::string output = "a.out";
  Stage stage = STAGE_BINARY;
  Mode mode = MODE_SAFE;
  std::string opt_level;

  // `shield test <path>` builds the package together with its `*_test.sword`
  // files, behind an entry point that runs them, then runs it.
  bool testing = argc > 1 && !strcmp(argv[1], "test");
  int first = testing ? 2 : 1;
  bool named = false; // whether -o asked for a particular path
  std::string forwarded; // options the test binary reads for itself
  std::vector<std::string> only; // `-run`: the tests to keep
  bool benching = false;         // `-bench`: run the benchmarks instead
  bool sim = false;              // `-sim` / `--sim`: built for the simulator
  uint64_t seeds = 100;          // `-seeds`: how many, from 1
  bool one_seed = false;         // `-seed`: just this one
  uint64_t seed = 0;
  if (testing) output = "";

  for (int i = first; i < argc; i++) {
    const char *arg = argv[i];
    if (!strcmp(arg, "-o") && i + 1 < argc) { output = argv[++i]; named = true; }
    else if (!strcmp(arg, "-I") && i + 1 < argc) search.push_back(argv[++i]);
    else if (!strcmp(arg, "--link") && i + 1 < argc)
      link_with.push_back(argv[++i]);
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
    else if (testing && !strcmp(arg, "-run") && i + 1 < argc)
      only.push_back(argv[++i]);
    else if (testing && !strcmp(arg, "-bench")) benching = true;
    else if (testing && !strcmp(arg, "-sim")) sim = true;
    else if (!testing && !strcmp(arg, "--sim")) sim = true;
    else if (testing && !strcmp(arg, "-seeds") && i + 1 < argc)
      seeds = strtoull(argv[++i], nullptr, 10);
    else if (testing && !strcmp(arg, "-seed") && i + 1 < argc) {
      one_seed = true;
      seed = strtoull(argv[++i], nullptr, 10);
    }
    else if (!strcmp(arg, "-h") || !strcmp(arg, "--help")) { usage(); return 0; }
    else if (!strcmp(arg, "--version")) {
      printf("shield %s (%s)\n", SWORD_VERSION, SWORD_RELEASE_NAME);
      return 0;
    }
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
  LoadMode how = LOAD_BUILD;
  if (testing) how = benching ? LOAD_BENCH : LOAD_TESTS;
  if (!load_program(input, search, prog, how, only))
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

  if (sim) warn_unsimulated(prog);

  IrModule mod;
  mod.sim_points = sim;
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
                     runtime_link_flags(), link_with);
  unlink(ll_path.c_str());
  if (!ok) return 1;
  if (!testing) return 0;

  if (sim) {
    int status = simulate(output, input, one_seed ? seed : 0,
                          one_seed ? 1 : seeds, one_seed);
    if (!named) unlink(output.c_str());
    return status;
  }

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
