#pragma once

#include "../src/package.h"

#include <string>
#include <vector>

// The legend the server advertises; the editor maps these onto its theme.
extern const std::vector<std::string> kTokenTypes;
extern const std::vector<std::string> kTokenModifiers;

struct SemToken {
  int line = 0; // 0-based
  int col = 0;  // 0-based, in UTF-16 code units as the protocol wants
  int length = 0;
  int type = 0;
  int modifiers = 0;
};

// Classifies every token of one file. Works on a partial parse, so a file with
// errors still comes back coloured.
std::vector<SemToken> semantic_tokens(int file, Program &prog);
