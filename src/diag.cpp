#include "diag.h"

#include <cstdarg>
#include <cstdio>
#include <deque>
#include <vector>

namespace {
int errors = 0;
std::deque<Source> sources;
std::vector<Diagnostic> *sink = nullptr;
} // namespace

int add_source(const std::string &path, std::string text) {
  Source src;
  src.path = path;
  src.text = std::move(text);

  src.line_start.push_back(0);
  for (size_t i = 0; i < src.text.size(); i++)
    if (src.text[i] == '\n') src.line_start.push_back(i + 1);

  sources.push_back(std::move(src));
  return (int)sources.size() - 1;
}

int load_source(const std::string &path) {
  FILE *f = fopen(path.c_str(), "rb");
  if (!f) return -1;

  std::string text;
  char buf[8192];
  size_t n;
  while ((n = fread(buf, 1, sizeof(buf), f)) > 0) text.append(buf, n);
  fclose(f);
  return add_source(path, std::move(text));
}

const Source &source_at(int index) { return sources[index]; }
int source_count() { return (int)sources.size(); }

void reset_sources() { sources.clear(); }
void reset_errors() { errors = 0; }
void capture_diagnostics(std::vector<Diagnostic> *where) { sink = where; }

std::string Source::line(int n) const {
  if (n < 1 || (size_t)n > line_start.size()) return "";
  size_t begin = line_start[n - 1];
  size_t end = text.find('\n', begin);
  if (end == std::string::npos) end = text.size();
  while (end > begin && text[end - 1] == '\r') end--;
  return text.substr(begin, end - begin);
}

// Prints a clang-style diagnostic: location, message, offending line, caret.
static void report(Pos pos, const char *kind, const char *fmt, va_list ap) {
  if (sink) {
    char buf[1024];
    vsnprintf(buf, sizeof(buf), fmt, ap);
    sink->push_back(Diagnostic{pos, buf, kind[0] == 'e'});
    return;
  }
  if (pos.file < 0 || (size_t)pos.file >= sources.size()) {
    fprintf(stderr, "shield: %s: ", kind);
    vfprintf(stderr, fmt, ap);
    fputc('\n', stderr);
    return;
  }
  const Source &src = sources[pos.file];

  fprintf(stderr, "%s:%d:%d: %s: ", src.path.c_str(), pos.line, pos.col, kind);
  vfprintf(stderr, fmt, ap);
  fputc('\n', stderr);

  std::string text = src.line(pos.line);
  if (text.empty()) return;
  fprintf(stderr, "  %5d | %s\n", pos.line, text.c_str());
  fprintf(stderr, "        | ");
  for (int i = 1; i < pos.col; i++)
    fputc(i - 1 < (int)text.size() && text[i - 1] == '\t' ? '\t' : ' ', stderr);
  fputs("^\n", stderr);
}

void error(Pos pos, const char *fmt, ...) {
  errors++;
  va_list ap;
  va_start(ap, fmt);
  report(pos, "error", fmt, ap);
  va_end(ap);
}

void note(Pos pos, const char *fmt, ...) {
  va_list ap;
  va_start(ap, fmt);
  report(pos, "note", fmt, ap);
  va_end(ap);
}

int error_count() { return errors; }
