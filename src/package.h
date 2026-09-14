#pragma once

#include "ast.h"
#include "types.h"

#include <deque>
#include <string>
#include <unordered_map>
#include <vector>

// A package is a directory: every .sword file in it shares one scope, so there
// are no headers and declaration order never matters.
struct Package {
  std::string import_path; // "std/mem"; empty for the package being compiled
  std::string name;        // "mem"; how importers refer to it
  std::string prefix;      // "std.mem."; empty for the main package
  std::string dir;
  Node *unit = nullptr;
  std::vector<std::string> imports;

  // Set when a file in this package mentions `chan`, which is what pulls in the
  // package that implements one.
  bool needs_chan = false;

  // Errors this package declares, by name, so a diagnostic can say where one
  // came from.
  std::unordered_map<std::string, int> error_codes;

  // Filled in by the checker. A name is exported when it starts uppercase.
  std::unordered_map<std::string, Symbol *> globals;
  std::unordered_map<std::string, Type *> type_names;
  // Monomorphized copies of this package's generic functions and struct
  // methods, kept apart from `unit` so instantiating one while checking it is
  // safe.
  std::vector<Node *> instances;

  // Templates: a generic struct is not a type until it is given arguments,
  // and its methods are not functions until then either.
  std::unordered_map<std::string, Node *> generic_types;
  std::unordered_map<std::string, std::vector<Node *>> generic_methods;
};

struct Program {
  Ast ast;
  std::deque<Package> packages;
  std::unordered_map<std::string, Package *> by_path;
  std::vector<Package *> order; // dependencies before dependents

  // Methods are program-wide, keyed by the qualified type name they hang off.
  std::unordered_map<std::string, std::unordered_map<std::string, Symbol *>>
      methods;

  // An instantiation is checked in the package that declares the generic, so
  // its body sees that package's own names.
  struct Instance {
    Package *owner;
    Node *decl;
    std::vector<std::pair<std::string, Type *>> bind;
  };
  std::vector<Instance> pending;
  std::unordered_map<std::string, Symbol *> instances;
  std::unordered_map<std::string, Type *> struct_instances;

  // Every enum gets a synthesized function from value to member name, which is
  // what `nameof` calls. Keyed by the type, because the caller may be in
  // another package.
  std::unordered_map<Type *, Symbol *> enum_names;

  // `nameof` over an error resolves to the first of these, `e.Message()` to the
  // second. Unlike an enum's, neither can be written as source in advance: the
  // set of errors is only complete once every package has been checked, so
  // lowering builds both bodies.
  Symbol *error_name = nullptr;
  Symbol *error_message = nullptr;

  Package *main() { return order.empty() ? nullptr : order.back(); }
};

bool exported(const std::string &name);

// The editor's buffer wins over what is on disk, so a file with unsaved edits
// is checked as the user sees it.
void set_overlay(const std::string &path, std::string text);
void clear_overlay(const std::string &path);

// What a load is for. A build leaves the `*_test.sword` files out; `shield test`
// takes them in and writes the entry point that runs their `Test*` functions.
// An editor wants the test files too — somebody is editing one — but not an
// entry point it never asked for, and no complaint about a package that has no
// `main` because it is a library.
enum LoadMode { LOAD_BUILD, LOAD_TESTS, LOAD_EDITOR };

// `input` is a directory (the whole package) or a single .sword file. Parses it
// and everything it imports, in dependency order.
bool load_program(const std::string &input,
                  const std::vector<std::string> &search, Program &out,
                  LoadMode mode = LOAD_BUILD);
