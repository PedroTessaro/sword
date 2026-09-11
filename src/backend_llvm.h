#pragma once

#include "ir.h"

#include <cstdio>

// Emits textual LLVM IR. Keeping the backend behind this one call is what
// makes swapping in the in-process LLVM API later a contained change.
void emit_llvm(const IrModule &mod, FILE *out);
