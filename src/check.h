#pragma once

#include "package.h"
#include "types.h"

// Checks every package in dependency order. Names are qualified with the
// package prefix as they are declared, so the rest of the compiler never has
// to think about packages again.
bool check(Program &prog, TypeTable &types);
