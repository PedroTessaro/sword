#include "sword_rt.h"

#include <errno.h>
#include <string.h>
#include <fcntl.h>
#include <sys/stat.h>
#include <unistd.h>

// Files, unlike sockets, are always "ready" as far as kqueue and epoll are
// concerned: asking a poller about a regular file tells you nothing, because
// the wait is the disk and not the arrival of anything. So every call here goes
// to a thread kept for the purpose — `sword_offload` — and the task that asked
// for it is put down until the answer comes back. What that costs is the size of
// that pool, not one thread per read in flight.
extern "C" {

namespace {

// Copies a path out of a Sword string, which is not terminated.
bool as_path(const char *path, int64_t path_len, char *into, size_t room) {
  if (path_len <= 0 || (size_t)path_len >= room) return false;
  memcpy(into, path, (size_t)path_len);
  into[path_len] = '\0';
  return true;
}

struct OpenCall {
  const char *name;
  int flags;
};

int64_t do_open(void *p) {
  OpenCall *c = (OpenCall *)p;
  return open(c->name, c->flags, 0644);
}

struct RwCall {
  int fd;
  void *buf;
  int64_t len;
};

int64_t do_read(void *p) {
  RwCall *c = (RwCall *)p;
  while (true) {
    ssize_t n = read(c->fd, c->buf, (size_t)c->len);
    if (n >= 0) return n;
    if (errno == EINTR) continue;
    return -1;
  }
}

// Short writes happen on a pipe as well as on a full disk; the caller wants all
// or nothing.
int64_t do_write(void *p) {
  RwCall *c = (RwCall *)p;
  int64_t sent = 0;
  while (sent < c->len) {
    ssize_t n = write(c->fd, (const char *)c->buf + sent, (size_t)(c->len - sent));
    if (n < 0) {
      if (errno == EINTR) continue;
      return -1;
    }
    if (n == 0) return -1;
    sent += n;
  }
  return sent;
}

struct PathCall {
  const char *name;
};

int64_t do_size(void *p) {
  struct stat info;
  if (stat(((PathCall *)p)->name, &info) != 0) return -1;
  if (S_ISDIR(info.st_mode)) return -2;
  return (int64_t)info.st_size;
}

int64_t do_remove(void *p) {
  return unlink(((PathCall *)p)->name) == 0 ? 0 : -1;
}

int64_t do_close(void *p) { return close(((RwCall *)p)->fd); }

} // namespace

enum {
  SWORD_FS_READ = 0,
  SWORD_FS_WRITE = 1,  // truncates, or creates
  SWORD_FS_APPEND = 2,
};

int32_t sword_fs_open(const char *path, int64_t path_len, int32_t mode) {
  char name[1024];
  if (!as_path(path, path_len, name, sizeof(name))) return -1;

  int flags = O_RDONLY;
  if (mode == SWORD_FS_WRITE) flags = O_WRONLY | O_CREAT | O_TRUNC;
  else if (mode == SWORD_FS_APPEND) flags = O_WRONLY | O_CREAT | O_APPEND;

  OpenCall call{name, flags};
  int64_t fd = sword_offload(do_open, &call);
  return fd < 0 ? -1 : (int32_t)fd;
}

int64_t sword_fs_read(int32_t fd, void *buf, int64_t len) {
  RwCall call{fd, buf, len};
  return sword_offload(do_read, &call);
}

int64_t sword_fs_write(int32_t fd, const void *buf, int64_t len) {
  RwCall call{fd, (void *)buf, len};
  return sword_offload(do_write, &call);
}

int32_t sword_fs_close(int32_t fd) {
  RwCall call{fd, nullptr, 0};
  return (int32_t)sword_offload(do_close, &call);
}

// -1 when it is not there or cannot be looked at, which the caller turns into
// an optional.
int64_t sword_fs_size(const char *path, int64_t path_len) {
  char name[1024];
  if (!as_path(path, path_len, name, sizeof(name))) return -1;
  PathCall call{name};
  return sword_offload(do_size, &call);
}

int32_t sword_fs_remove(const char *path, int64_t path_len) {
  char name[1024];
  if (!as_path(path, path_len, name, sizeof(name))) return -1;
  PathCall call{name};
  return (int32_t)sword_offload(do_remove, &call);
}
}
