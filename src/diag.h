#pragma once

#include <cstdint>
#include <string>
#include <vector>

// A position carries the file it came from, so a diagnostic can find its
// source without every pass having to pass one around.
struct Pos {
  int file = 0;
  int line = 1;
  int col = 1;
};

struct Source {
  std::string path;
  std::string text;
  std::vector<size_t> line_start;

  std::string line(int n) const;
};

// Returns the index of the loaded file, or -1 if it could not be read.
int load_source(const std::string &path);
// Same, but with the text already in hand: the language server works from the
// editor's buffer, which may differ from what is on disk.
int add_source(const std::string &path, std::string text);
const Source &source_at(int index);
int source_count();
void reset_sources();

void error(Pos pos, const char *fmt, ...);
void note(Pos pos, const char *fmt, ...);
int error_count();
void reset_errors();

// Collected instead of printed while a sink is installed. Passing null goes
// back to writing on stderr.
struct Diagnostic {
  Pos pos;
  std::string message;
  bool is_error = true;
};
void capture_diagnostics(std::vector<Diagnostic> *sink);
