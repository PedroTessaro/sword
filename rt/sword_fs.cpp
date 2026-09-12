#include "sword_rt.h"

#include <errno.h>
#include <string.h>
#include <fcntl.h>
#include <sys/stat.h>
#include <unistd.h>

// Files, unlike sockets, are always "ready" as far as kqueue and epoll are
// concerned: asking a poller about a regular file tells you nothing, because
// the wait is the disk and not the arrival of anything. So this goes the other
// way — the thread really does stop, and the scheduler is told so it can hire a
// replacement while it is gone. That is what the blocking hints were built for
// and what they are still the right answer to.
extern "C" {

struct Parked {
  Parked() { sword_blocking_enter(); }
  ~Parked() { sword_blocking_exit(); }
};

enum {
  SWORD_FS_READ = 0,
  SWORD_FS_WRITE = 1,  // truncates, or creates
  SWORD_FS_APPEND = 2,
};

int32_t sword_fs_open(const char *path, int64_t path_len, int32_t mode) {
  char name[1024];
  if (path_len <= 0 || path_len >= (int64_t)sizeof(name)) return -1;
  memcpy(name, path, (size_t)path_len);
  name[path_len] = '\0';

  int flags = O_RDONLY;
  if (mode == SWORD_FS_WRITE) flags = O_WRONLY | O_CREAT | O_TRUNC;
  else if (mode == SWORD_FS_APPEND) flags = O_WRONLY | O_CREAT | O_APPEND;

  Parked parked;
  int fd = open(name, flags, 0644);
  return fd < 0 ? -1 : fd;
}

int64_t sword_fs_read(int32_t fd, void *buf, int64_t len) {
  Parked parked;
  while (true) {
    ssize_t n = read(fd, buf, (size_t)len);
    if (n >= 0) return n;
    if (errno == EINTR) continue;
    return -1;
  }
}

// Short writes happen on a pipe as well as on a full disk; the caller wants all
// or nothing.
int64_t sword_fs_write(int32_t fd, const void *buf, int64_t len) {
  Parked parked;
  int64_t sent = 0;
  while (sent < len) {
    ssize_t n = write(fd, (const char *)buf + sent, (size_t)(len - sent));
    if (n < 0) {
      if (errno == EINTR) continue;
      return -1;
    }
    if (n == 0) return -1;
    sent += n;
  }
  return sent;
}

int32_t sword_fs_close(int32_t fd) { return close(fd); }

// -1 when it is not there or cannot be looked at, which the caller turns into
// an optional.
int64_t sword_fs_size(const char *path, int64_t path_len) {
  char name[1024];
  if (path_len <= 0 || path_len >= (int64_t)sizeof(name)) return -1;
  memcpy(name, path, (size_t)path_len);
  name[path_len] = '\0';

  Parked parked;
  struct stat info;
  if (stat(name, &info) != 0) return -1;
  if (S_ISDIR(info.st_mode)) return -2;
  return (int64_t)info.st_size;
}

int32_t sword_fs_remove(const char *path, int64_t path_len) {
  char name[1024];
  if (path_len <= 0 || path_len >= (int64_t)sizeof(name)) return -1;
  memcpy(name, path, (size_t)path_len);
  name[path_len] = '\0';

  Parked parked;
  return unlink(name) == 0 ? 0 : -1;
}
}
