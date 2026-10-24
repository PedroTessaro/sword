#pragma once

#include "diag.h"

#include <string>
#include <vector>

// A module is a tree of packages with a sword.mod at its root, named by the
// path it is imported under: `github.com/ana/sqlite`, and its packages below it
// (`github.com/ana/sqlite/pool`).
struct Module {
  std::string path; // what `module` says
  std::string root; // the directory sword.mod is in
  int sword_major = 0, sword_minor = 0;

  struct Require {
    std::string path;
    std::string version;
    Pos at;
  };
  std::vector<Require> required;

  // `replace path => dir`, with dir made absolute against the module's root.
  struct Replace {
    std::string path;
    std::string dir;
    Pos at;
  };
  std::vector<Replace> replaces;

  // The directory an import path names inside this module or through one of its
  // replacements, or empty when it is neither.
  std::string resolve(const std::string &import_path) const;
};

// Looks for sword.mod in `dir` and every directory above it. Answers false only
// when one was found and could not be read; `found` says whether there was one.
bool find_module(const std::string &dir, Module &out, bool &found);

// Whether `version` reads as a version: v1.4.0, or a pseudo-version for a
// commit with no tag, v0.0.0-20261023-a1b2c3d.
bool valid_version(const std::string &version);

// The package name an import path gives, which is its last element unless that
// is a major version (`.../sqlite/v2` is package sqlite).
std::string package_name_of(const std::string &import_path);
