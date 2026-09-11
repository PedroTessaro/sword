#pragma once

#include <string>
#include <vector>

// Where the compiler's own files live. Two layouts have to work: the build
// tree, where everything sits next to the binary, and an installed tree, where
// the binary is in bin/ and its files are in ../lib and ../share.
std::string executable_path();
std::string directory_of(const std::string &path);

// Roots to look for imported packages in, most specific first.
std::vector<std::string> package_roots();

// The runtime archive linked into every compiled program.
std::string runtime_archive();
