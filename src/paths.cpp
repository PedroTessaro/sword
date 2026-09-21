#include "paths.h"

#include <cstdio>
#include <cstdlib>
#include <sys/stat.h>
#include <unistd.h>

#ifdef __APPLE__
#include <mach-o/dyld.h>
#endif
#ifndef PATH_MAX
#define PATH_MAX 4096
#endif

namespace {

bool exists(const std::string &path) {
  struct stat info;
  return stat(path.c_str(), &info) == 0;
}

} // namespace

std::string directory_of(const std::string &path) {
  size_t cut = path.find_last_of('/');
  return cut == std::string::npos ? std::string(".") : path.substr(0, cut);
}

// argv[0] is not enough: invoked through PATH it carries no directory at all.
std::string executable_path() {
  char resolved[PATH_MAX];

#ifdef __APPLE__
  uint32_t size = sizeof(resolved);
  char raw[PATH_MAX];
  if (_NSGetExecutablePath(raw, &size) == 0) {
    if (realpath(raw, resolved)) return resolved;
    return raw;
  }
#else
  ssize_t written = readlink("/proc/self/exe", resolved, sizeof(resolved) - 1);
  if (written > 0) {
    resolved[written] = '\0';
    return resolved;
  }
#endif
  return "";
}

std::vector<std::string> package_roots() {
  std::vector<std::string> roots;
  if (const char *from_env = getenv("SWORD_ROOT")) roots.push_back(from_env);

  std::string bin = directory_of(executable_path());
  roots.push_back(bin);                     // running from the build tree
  roots.push_back(bin + "/../share/sword"); // installed
  return roots;
}

std::string runtime_archive() {
  std::string bin = directory_of(executable_path());
  const std::string candidates[] = {
      bin + "/libsword_rt.a",
      bin + "/../lib/sword/libsword_rt.a",
  };
  for (const std::string &path : candidates)
    if (exists(path)) return path;
  // Nothing to link is not a failure: a program that never spawns a task does
  // not reference the scheduler at all.
  return "";
}

// The runtime may have been built against a library only the build knew how to
// find — OpenSSL, for TLS. The archive carries its own link line in a file
// beside it rather than having the compiler guess at prefixes.
std::string runtime_link_flags() {
  std::string archive = runtime_archive();
  if (archive.empty()) return "";
  std::string path = archive.substr(0, archive.size() - 2) + ".flags";
  FILE *file = fopen(path.c_str(), "r");
  if (!file) return "";
  std::string flags;
  char chunk[512];
  while (fgets(chunk, sizeof(chunk), file)) flags += chunk;
  fclose(file);
  // A second line is another argument, not another command. What comes back
  // here is pasted into the clang invocation and run through a shell, so a
  // newline left in the middle would end that command and run the rest of the
  // file as one of its own.
  for (char &c : flags)
    if (c == '\n' || c == '\r') c = ' ';
  while (!flags.empty() && flags.back() == ' ') flags.pop_back();
  return flags;
}
