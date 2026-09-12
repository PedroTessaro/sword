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

bool is_test_file(const std::string &name) {
  const std::string suffix = "_test.sw";
  return name.size() > suffix.size() &&
         name.compare(name.size() - suffix.size(), suffix.size(), suffix) == 0;
}

std::vector<std::string> sword_files(const std::string &dir, bool with_tests) {
  std::vector<std::string> files;
  DIR *handle = opendir(dir.c_str());
  if (!handle) return files;
  while (dirent *entry = readdir(handle)) {
    std::string name = entry->d_name;
    if (name.size() < 4 || name.compare(name.size() - 3, 3, ".sw") != 0)
      continue;
    // Tests live beside what they test, and an ordinary build must not drag
    // them into every program that imports the package.
    if (!with_tests && is_test_file(name)) continue;
    files.push_back(dir + "/" + name);
  }
  closedir(handle);
  // Directory order is not stable across filesystems; sort so that diagnostics
  // and generated code come out the same every time.
  std::sort(files.begin(), files.end());
  return files;
}

struct Loader {
  Program &prog;
  const std::vector<std::string> &search;
  std::vector<std::string> visiting;
  bool tests = false;
  std::string label; // what the user named on the command line

  Loader(Program &p, const std::vector<std::string> &s) : prog(p), search(s) {}

  // The entry point of a test build, written as source and handed to the same
  // parser as everything else. Generating text rather than nodes keeps this
  // honest: whatever the language accepts, this has to be written in.
  bool add_test_main(Package &pkg) {
    std::vector<std::string> found;
    for (Node *decl : pkg.unit->kids) {
      if (decl->kind != ND_FUNC || decl->lhs || decl->is_extern) continue;
      // `Test` on its own is not a test, and a test takes exactly the one
      // parameter. Whether it is the right type is the checker's business.
      if (decl->name.size() <= 4 || decl->name.compare(0, 4, "Test") != 0)
        continue;
      if (decl->kids.size() != 1) continue;
      found.push_back(decl->name);
    }
    if (found.empty()) {
      fprintf(stderr, "shield: no 'Test...' functions in '%s'\n",
              label.empty() ? pkg.dir.c_str() : label.c_str());
      return false;
    }

    // Testing a program means not running it: its own main is renamed out of
    // reach rather than colliding with the one below.
    for (Node *decl : pkg.unit->kids)
      if (decl->kind == ND_FUNC && !decl->lhs && decl->name == "main")
        decl->name = "main.under test";

    std::string src = "import \"std/testing\"\n\nfunc main() !int {\n";
    src += "    mut cases := [" + std::to_string(found.size()) +
           "]testing.Case{";
    for (size_t i = 0; i < found.size(); i++) {
      src += i ? ",\n        " : "\n        ";
      src += "testing.NewCase(\"" + found[i] + "\", " + found[i] + ")";
    }
    src += "}\n    return try testing.Run(cases[..])\n}\n";

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
      fprintf(stderr, "shield: no .sw files in '%s'\n", pkg.dir.c_str());
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
      if (!parse(tokens, prog.ast, pkg.unit)) return false;
    }

    for (Node *decl : pkg.unit->kids)
      if (decl->kind == ND_IMPORT) pkg.imports.push_back(decl->text);
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

    if (!parse_files(pkg, sword_files(dir, tests && import_path.empty())))
      return nullptr;
    if (tests && import_path.empty() && !add_test_main(pkg)) return nullptr;

    for (Node *decl : pkg.unit->kids) {
      if (decl->kind != ND_IMPORT) continue;
      std::string found_dir = locate(decl->text);
      if (found_dir.empty()) {
        error(decl->pos, "cannot find package '%s'", decl->text.c_str());
        return nullptr;
      }
      if (!load(decl->text, found_dir, decl->pos)) return nullptr;
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
                  bool with_tests) {
  Loader loader(out, search);
  loader.tests = with_tests;
  loader.label = input;

  if (is_directory(input)) return loader.load("", input, Pos{}) != nullptr;

  // A single file is its own package: compiling one file must not silently
  // pull in its neighbours.
  out.packages.emplace_back();
  Package &pkg = out.packages.back();
  pkg.dir = parent_of(input);
  pkg.prefix = "main.";
  if (!loader.parse_files(pkg, {input})) return false;
  if (with_tests && !loader.add_test_main(pkg)) return false;

  for (Node *decl : pkg.unit->kids) {
    if (decl->kind != ND_IMPORT) continue;
    std::string dir = loader.locate(decl->text);
    if (dir.empty()) {
      error(decl->pos, "cannot find package '%s'", decl->text.c_str());
      return false;
    }
    if (!loader.load(decl->text, dir, decl->pos)) return false;
  }

  out.by_path[""] = &pkg;
  out.order.push_back(&pkg);
  return true;
}
