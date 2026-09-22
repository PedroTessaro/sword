#include "package.h"

#include "lexer.h"
#include "parser.h"

#include <algorithm>
#include <cctype>
#include <dirent.h>
#include <sys/stat.h>

namespace {

std::unordered_map<std::string, std::string> overlays;

bool is_directory(const std::string &path) {
  struct stat info;
  return stat(path.c_str(), &info) == 0 && S_ISDIR(info.st_mode);
}

std::string parent_of(const std::string &path) {
  size_t cut = path.find_last_of('/');
  return cut == std::string::npos ? std::string(".") : path.substr(0, cut);
}

std::string last_segment(const std::string &path) {
  size_t cut = path.find_last_of('/');
  return cut == std::string::npos ? path : path.substr(cut + 1);
}

bool ends_with(const std::string &name, const std::string &suffix) {
  return name.size() > suffix.size() &&
         name.compare(name.size() - suffix.size(), suffix.size(), suffix) == 0;
}

bool is_test_file(const std::string &name) {
  return ends_with(name, "_test.sword");
}

// Sword files used to end in .sw, which GitHub counts as Sway. One left behind
// would otherwise be skipped without a word, and whatever it declared would
// turn up as undefined somewhere else, so it stops the build and says why.
bool reject_stale(const std::string &path) {
  if (!ends_with(path, ".sw")) return false;
  fprintf(stderr, "shield: '%s' ends in .sw; Sword files end in .sword\n",
          path.c_str());
  return true;
}

// The package's files, or false when a file in the directory was refused.
bool sword_files(const std::string &dir, bool with_tests,
                 std::vector<std::string> &files) {
  std::vector<std::string> stale;
  DIR *handle = opendir(dir.c_str());
  if (!handle) return true;
  while (dirent *entry = readdir(handle)) {
    std::string name = entry->d_name;
    if (ends_with(name, ".sw")) stale.push_back(dir + "/" + name);
    if (!ends_with(name, ".sword")) continue;
    // Tests live beside what they test, and an ordinary build must not drag
    // them into every program that imports the package.
    if (!with_tests && is_test_file(name)) continue;
    files.push_back(dir + "/" + name);
  }
  closedir(handle);
  // Directory order is not stable across filesystems; sort so that diagnostics
  // and generated code come out the same every time.
  std::sort(files.begin(), files.end());
  std::sort(stale.begin(), stale.end());
  return stale.empty() || !reject_stale(stale.front());
}

struct Loader {
  Program &prog;
  const std::vector<std::string> &search;
  std::vector<std::string> visiting;
  LoadMode mode = LOAD_BUILD;
  std::string label; // what the user named on the command line
  std::vector<std::string> only; // the tests asked for by name; empty is all

  Loader(Program &p, const std::vector<std::string> &s) : prog(p), search(s) {}

  // The entry point of a test build, written as source and handed to the same
  // parser as everything else. Generating text rather than nodes keeps this
  // honest: whatever the language accepts, this has to be written in.
  bool add_test_main(Package &pkg) {
    // The same entry point either way: a benchmark is a function found by its
    // name, handed to a runner, like a test.
    bool benching = mode == LOAD_BENCH;
    const std::string prefix = benching ? "Benchmark" : "Test";
    const char *what = benching ? "benchmark" : "test";

    std::vector<std::string> found;
    for (Node *decl : pkg.unit->kids) {
      if (decl->kind != ND_FUNC || decl->lhs || decl->is_extern) continue;
      // The prefix on its own is not one of them, and each takes exactly the
      // one parameter. Whether it is the right type is the checker's business.
      if (decl->name.size() <= prefix.size() ||
          decl->name.compare(0, prefix.size(), prefix) != 0)
        continue;
      if (decl->kids.size() != 1) continue;
      found.push_back(decl->name);
    }
    if (found.empty()) {
      fprintf(stderr, "shield: no '%s...' functions in '%s'\n",
              prefix.c_str(), label.empty() ? pkg.dir.c_str() : label.c_str());
      return false;
    }
    // A name that matches nothing is a mistake, not a run of zero tests that
    // passes: a typo must not look like success.
    for (const std::string &name : only) {
      if (std::find(found.begin(), found.end(), name) != found.end()) continue;
      fprintf(stderr, "shield: no %s '%s' in '%s'\n", what, name.c_str(),
              label.empty() ? pkg.dir.c_str() : label.c_str());
      return false;
    }
    if (!only.empty()) {
      std::vector<std::string> picked;
      for (const std::string &name : found)
        if (std::find(only.begin(), only.end(), name) != only.end())
          picked.push_back(name);
      found = picked;
    }

    // Testing a program means not running it: its own main is renamed out of
    // reach rather than colliding with the one below.
    for (Node *decl : pkg.unit->kids)
      if (decl->kind == ND_FUNC && !decl->lhs && decl->name == "main")
        decl->name = "main.under test";

    const char *kind = benching ? "Bench" : "Case";
    const char *make = benching ? "NewBench" : "NewCase";
    const char *run = benching ? "RunBenches" : "Run";
    std::string src = "import \"std/testing\"\n\nfunc main() !int {\n";
    src += "    mut cases := [" + std::to_string(found.size()) + "]testing." +
           kind + "{";
    for (size_t i = 0; i < found.size(); i++) {
      src += i ? ",\n        " : "\n        ";
      src += std::string("testing.") + make + "(\"" + found[i] + "\", " +
             found[i] + ")";
    }
    src += "}\n    return try testing." + std::string(run) +
           "(cases[..])\n}\n";

    int id = add_source("<test main>", src);
    std::vector<Token> tokens = lex(id);
    if (error_count() > 0) return false;
    if (!parse(tokens, prog.ast, pkg.unit)) return false;

    for (Node *decl : pkg.unit->kids)
      if (decl->kind == ND_IMPORT && decl->text == "std/testing")
        pkg.imports.push_back(decl->text);
    return true;
  }

  bool parse_files(Package &pkg, const std::vector<std::string> &files) {
    if (files.empty()) {
      fprintf(stderr, "shield: no .sword files in '%s'\n", pkg.dir.c_str());
      return false;
    }
    pkg.unit = prog.ast.make(ND_UNIT, Pos{});
    for (const std::string &file : files) {
      auto edited = overlays.find(file);
      int id = edited != overlays.end() ? add_source(file, edited->second)
                                        : load_source(file);
      if (id < 0) {
        fprintf(stderr, "shield: cannot open '%s'\n", file.c_str());
        return false;
      }
      std::vector<Token> tokens = lex(id);
      if (error_count() > 0) return false;
      // `chan[T]` is written like a type and implemented like a package, and
      // nobody should have to import it to use a language feature. Noticing the
      // word here is enough: the import is added below, and an extra one for a
      // file that only mentions `chan` in passing costs a package nobody calls.
      for (const Token &t : tokens)
        if (t.kind == TK_IDENT && t.text == "chan") pkg.needs_chan = true;
      if (!parse(tokens, prog.ast, pkg.unit)) return false;
    }

    for (Node *decl : pkg.unit->kids)
      if (decl->kind == ND_IMPORT) pkg.imports.push_back(decl->text);
    if (pkg.needs_chan && pkg.import_path != "std/chan")
      pkg.imports.push_back("std/chan");
    return true;
  }

  std::string locate(const std::string &import_path) {
    for (const std::string &root : search) {
      std::string candidate = root + "/" + import_path;
      if (is_directory(candidate)) return candidate;
    }
    return "";
  }

  // Depth-first, appending each package after its dependencies.
  Package *load(const std::string &import_path, const std::string &dir,
                Pos at) {
    auto found = prog.by_path.find(import_path);
    if (found != prog.by_path.end()) return found->second;

    for (const std::string &open : visiting) {
      if (open != import_path) continue;
      error(at, "import cycle through '%s'", import_path.c_str());
      return nullptr;
    }
    visiting.push_back(import_path);

    prog.packages.emplace_back();
    Package &pkg = prog.packages.back();
    pkg.import_path = import_path;
    pkg.dir = dir;
    // The program's own package is qualified too. Without a prefix its
    // functions land in the object file under the names they were written
    // with, and a program with a function called `read` or `shutdown` would
    // have the runtime call that instead of the one in libc.
    pkg.prefix = "main.";
    if (!import_path.empty()) {
      pkg.name = last_segment(import_path);
      pkg.prefix = import_path + ".";
      for (char &c : pkg.prefix)
        if (c == '/') c = '.';
    }

    std::vector<std::string> files;
    if (!sword_files(dir, mode != LOAD_BUILD && import_path.empty(), files) ||
        !parse_files(pkg, files))
      return nullptr;
    if ((mode == LOAD_TESTS || mode == LOAD_BENCH) &&
        import_path.empty() && !add_test_main(pkg))
      return nullptr;

    for (const std::string &path : pkg.imports) {
      std::string found_dir = locate(path);
      if (found_dir.empty()) {
        Pos at = pkg.unit->kids.empty() ? Pos{} : pkg.unit->kids.front()->pos;
        for (Node *decl : pkg.unit->kids)
          if (decl->kind == ND_IMPORT && decl->text == path) at = decl->pos;
        error(at, "cannot find package '%s'", path.c_str());
        return nullptr;
      }
      if (!load(path, found_dir, Pos{})) return nullptr;
    }

    visiting.pop_back();
    prog.by_path[import_path] = &pkg;
    prog.order.push_back(&pkg);
    return &pkg;
  }
};

} // namespace

void set_overlay(const std::string &path, std::string text) {
  overlays[path] = std::move(text);
}

void clear_overlay(const std::string &path) { overlays.erase(path); }

bool exported(const std::string &name) {
  return !name.empty() && isupper((unsigned char)name[0]);
}

bool load_program(const std::string &input,
                  const std::vector<std::string> &search, Program &out,
                  LoadMode mode, const std::vector<std::string> &only) {
  Loader loader(out, search);
  loader.mode = mode;
  loader.label = input;
  loader.only = only;

  if (is_directory(input)) return loader.load("", input, Pos{}) != nullptr;

  // A single file is its own package: compiling one file must not silently
  // pull in its neighbours.
  if (reject_stale(input)) return false;
  out.packages.emplace_back();
  Package &pkg = out.packages.back();
  pkg.dir = parent_of(input);
  pkg.prefix = "main.";
  if (!loader.parse_files(pkg, {input})) return false;
  if ((mode == LOAD_TESTS || mode == LOAD_BENCH) &&
      !loader.add_test_main(pkg))
    return false;

  for (const std::string &path : pkg.imports) {
    std::string dir = loader.locate(path);
    if (dir.empty()) {
      Pos at{};
      for (Node *decl : pkg.unit->kids)
        if (decl->kind == ND_IMPORT && decl->text == path) at = decl->pos;
      error(at, "cannot find package '%s'", path.c_str());
      return false;
    }
    if (!loader.load(path, dir, Pos{})) return false;
  }

  out.by_path[""] = &pkg;
  out.order.push_back(&pkg);
  return true;
}
