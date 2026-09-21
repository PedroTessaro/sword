#include "sword_rt.h"

#include <dirent.h>
#include <errno.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <fcntl.h>
#include <sys/stat.h>
#include <time.h>
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

int64_t do_mkdir(void *p) {
  return mkdir(((PathCall *)p)->name, 0755) == 0 ? 0 : -1;
}

int64_t do_rmdir(void *p) {
  return rmdir(((PathCall *)p)->name) == 0 ? 0 : -1;
}

int64_t do_close(void *p) { return close(((RwCall *)p)->fd); }

// Everything below answers with its own negative codes rather than leaving the
// caller to read errno: the call ran on another thread, and errno belongs to
// the thread that set it.

struct StatCall {
  const char *name;
  int64_t size;
  int64_t modified_ns;
  uint32_t mode;
  int32_t is_dir;
};

int64_t do_stat(void *p) {
  StatCall *c = (StatCall *)p;
  struct stat info;
  if (stat(c->name, &info) != 0) return -1;
  c->size = (int64_t)info.st_size;
  c->mode = (uint32_t)(info.st_mode & 07777);
  c->is_dir = S_ISDIR(info.st_mode) ? 1 : 0;
#ifdef __APPLE__
  c->modified_ns = (int64_t)info.st_mtimespec.tv_sec * 1000000000 +
                   info.st_mtimespec.tv_nsec;
#else
  c->modified_ns = (int64_t)info.st_mtim.tv_sec * 1000000000 +
                   info.st_mtim.tv_nsec;
#endif
  return 0;
}

struct RenameCall {
  const char *from;
  const char *to;
};

int64_t do_rename(void *p) {
  RenameCall *c = (RenameCall *)p;
  return rename(c->from, c->to) == 0 ? 0 : -1;
}

struct DirCall {
  const char *name;
  DIR *dir;
  char *into;
  int64_t room;
  int32_t is_dir;
};

int64_t do_opendir(void *p) {
  DirCall *c = (DirCall *)p;
  c->dir = opendir(c->name);
  return c->dir ? 0 : -1;
}

int64_t do_readdir(void *p) {
  DirCall *c = (DirCall *)p;
  while (true) {
    errno = 0;
    struct dirent *entry = readdir(c->dir);
    // Nothing left, or a failure — and on this thread errno still means
    // something, which is why the two are told apart here.
    if (!entry) return errno == 0 ? 0 : -1;
    const char *name = entry->d_name;
    // "." and ".." are the directory and its parent. Every caller has to skip
    // them, so skipping them here means nobody forgets.
    if (name[0] == '.' && (name[1] == '\0' ||
                           (name[1] == '.' && name[2] == '\0')))
      continue;
    size_t len = strlen(name);
    if ((int64_t)len > c->room) return -2; // the caller's buffer is too small
    memcpy(c->into, name, len);
    if (entry->d_type == DT_UNKNOWN) {
      // Some filesystems do not fill it in. Asking beside the open directory
      // needs no path of our own.
      struct stat info;
      c->is_dir = fstatat(dirfd(c->dir), name, &info, 0) == 0 &&
                          S_ISDIR(info.st_mode)
                      ? 1
                      : 0;
    } else {
      c->is_dir = entry->d_type == DT_DIR ? 1 : 0;
    }
    return (int64_t)len;
  }
}

int64_t do_closedir(void *p) { return closedir(((DirCall *)p)->dir); }

struct TempCall {
  const char *prefix;
  char *into;
  int64_t room;
};

// A name beside whatever the prefix names, created with O_EXCL so that the
// answer is a path nobody else holds. Making the file is the point: a name
// that is only unlikely to be taken is a race the caller cannot see.
int64_t do_temp(void *p) {
  TempCall *c = (TempCall *)p;
  size_t head = strlen(c->prefix);
  if ((int64_t)(head + 8) > c->room) return -2;

  static const char alphabet[] = "abcdefghijklmnopqrstuvwxyz0123456789";
  char path[1100];
  if (head + 8 >= sizeof(path)) return -2;
  memcpy(path, c->prefix, head);
  path[head + 7] = '\0';

  timespec now;
  clock_gettime(CLOCK_REALTIME, &now);
  uint64_t seed = (uint64_t)now.tv_nsec * 1000003 + (uint64_t)getpid();
  for (int attempt = 0; attempt < 128; attempt++) {
    seed = seed * 6364136223846793005ull + 1442695040888963407ull;
    uint64_t bits = seed >> 17;
    for (int i = 0; i < 7; i++) {
      path[head + i] = alphabet[bits % 36];
      bits /= 36;
    }
    int fd = open(path, O_RDWR | O_CREAT | O_EXCL, 0600);
    if (fd >= 0) {
      close(fd);
      memcpy(c->into, path, head + 7);
      return (int64_t)(head + 7);
    }
    if (errno != EEXIST) return -1;
  }
  return -1;
}

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

// A directory that is already there is not an error: the caller asked for it to
// exist, and it does.
int32_t sword_fs_make_dir(const char *path, int64_t path_len) {
  char name[1024];
  if (!as_path(path, path_len, name, sizeof(name))) return -1;
  PathCall call{name};
  if (sword_offload(do_mkdir, &call) == 0) return 0;
  return errno == EEXIST ? 0 : -1;
}

// Only an empty one, which is deliberate: removing a tree is a decision, not a
// convenience, and nothing here should make it a one-liner by accident.
int32_t sword_fs_remove_dir(const char *path, int64_t path_len) {
  char name[1024];
  if (!as_path(path, path_len, name, sizeof(name))) return -1;
  PathCall call{name};
  return (int32_t)sword_offload(do_rmdir, &call);
}

// Size, kind, mode and when it last changed, in one look. Folding all of it
// into a size and a nil is what left "missing", "a directory" and "cannot be
// read" looking the same.
int32_t sword_fs_stat(const char *path, int64_t path_len, int64_t *size,
                      int64_t *modified_ns, uint32_t *mode, int32_t *is_dir) {
  char name[1024];
  if (!as_path(path, path_len, name, sizeof(name))) return -1;
  StatCall call{name, 0, 0, 0, 0};
  if (sword_offload(do_stat, &call) != 0) return -1;
  *size = call.size;
  *modified_ns = call.modified_ns;
  *mode = call.mode;
  *is_dir = call.is_dir;
  return 0;
}

int32_t sword_fs_rename(const char *from, int64_t from_len, const char *to,
                        int64_t to_len) {
  char a[1024], b[1024];
  if (!as_path(from, from_len, a, sizeof(a))) return -1;
  if (!as_path(to, to_len, b, sizeof(b))) return -1;
  RenameCall call{a, b};
  return (int32_t)sword_offload(do_rename, &call);
}

// A directory is read one entry at a time, each on a thread of its own the way
// a file read is: the whole listing in one call would mean the runtime
// deciding how much memory it takes, and this library does not do that.
int64_t sword_fs_dir_open(const char *path, int64_t path_len) {
  char name[1024];
  if (!as_path(path, path_len, name, sizeof(name))) return -1;
  DirCall call{name, nullptr, nullptr, 0, 0};
  if (sword_offload(do_opendir, &call) != 0) return -1;
  return (int64_t)(intptr_t)call.dir;
}

// The name's length, 0 at the end, -1 for a failure and -2 when the name does
// not fit in what the caller offered.
int64_t sword_fs_dir_next(int64_t handle, char *into, int64_t room,
                          int32_t *is_dir) {
  DirCall call{nullptr, (DIR *)(intptr_t)handle, into, room, 0};
  int64_t n = sword_offload(do_readdir, &call);
  *is_dir = call.is_dir;
  return n;
}

void sword_fs_dir_close(int64_t handle) {
  DirCall call{nullptr, (DIR *)(intptr_t)handle, nullptr, 0, 0};
  sword_offload(do_closedir, &call);
}

// The path of a file this call has just made, beside whatever the prefix
// names. -2 when it does not fit in the caller's buffer.
int64_t sword_fs_temp_path(const char *prefix, int64_t prefix_len, char *into,
                           int64_t room) {
  char head[1024];
  if (!as_path(prefix, prefix_len, head, sizeof(head))) return -1;
  TempCall call{head, into, room};
  return sword_offload(do_temp, &call);
}
}
