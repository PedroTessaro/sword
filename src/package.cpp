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

std::vector<std::string> sword_files(const std::string &dir) {
  std::vector<std::string> files;
  DIR *handle = opendir(dir.c_str());
  if (!handle) return files;
  while (dirent *entry = readdir(handle)) {
    std::string name = entry->d_name;
    if (name.size() < 4 || name.compare(name.size() - 3, 3, ".sw") != 0)
      continue;
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

  Loader(Program &p, const std::vector<std::string> &s) : prog(p), search(s) {}

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
    if (!import_path.empty()) {
      pkg.name = last_segment(import_path);
      pkg.prefix = import_path + ".";
      for (char &c : pkg.prefix)
        if (c == '/') c = '.';
    }

    if (!parse_files(pkg, sword_files(dir))) return nullptr;

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
                  const std::vector<std::string> &search, Program &out) {
  Loader loader(out, search);

  if (is_directory(input)) return loader.load("", input, Pos{}) != nullptr;

  // A single file is its own package: compiling one file must not silently
  // pull in its neighbours.
  out.packages.emplace_back();
  Package &pkg = out.packages.back();
  pkg.dir = parent_of(input);
  if (!loader.parse_files(pkg, {input})) return false;

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
