#pragma once

#include "ir.h"
#include "package.h"
#include "types.h"

// Build modes trade safety for speed, the way Zig's release modes do.
enum Mode {
  MODE_DEBUG, // checks on, no optimization
  MODE_SAFE,  // checks on, optimized
  MODE_FAST,  // checks off
  MODE_SMALL, // checks off, optimized for size
};

inline bool mode_checks(Mode mode) {
  return mode == MODE_DEBUG || mode == MODE_SAFE;
}

void lower(Program &prog, TypeTable &types, Mode mode, IrModule &mod);
