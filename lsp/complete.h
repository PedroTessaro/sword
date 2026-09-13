#pragma once

#include "../src/package.h"
#include "../src/types.h"

#include <string>
#include <vector>

// One thing the editor can offer. `kind` is an LSP CompletionItemKind, which is
// what decides the icon beside the name; `detail` is the type or signature, shown
// to the right of it.
struct Completion {
  std::string label;
  std::string detail;
  int kind = 1;
};

// What could come next at this point in the file. `line` is the text of the line
// up to the cursor — everything after it is irrelevant to what is being typed,
// and the buffer may not parse yet, which is exactly when completion is wanted.
std::vector<Completion> completions(const std::string &before, int file,
                                    int line_number, Program &prog,
                                    TypeTable &types);
