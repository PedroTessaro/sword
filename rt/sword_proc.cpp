#include "sword_rt.h"

#include <errno.h>
#include <signal.h>
#include <spawn.h>
#include <stdint.h>
#include <string.h>
#include <sys/wait.h>
#include <unistd.h>

// Starting a program and waiting for it both stop the thread that does them,
// and neither is something a poller has anything to say about — so both go to
// the io pool through `sword_offload`, and the task that asked is put down the
// way a file read puts it down.
extern "C" {

extern char **environ;

namespace {

// argv comes across as one run of NUL-separated strings and a count, rather
// than as an array of pointers: a Sword string is not terminated, so the
// copying has to happen somewhere, and this way the FFI stays scalar.
const int kMaxArgs = 256;

struct SpawnCall {
  const char *blob;
  int64_t count;
  int32_t pipes;
  int32_t fds[3];
  int64_t pid;
};

// Both ends of one pipe, and which of them the child gets.
struct Pipe {
  int ours = -1;
  int theirs = -1;
};

void shut(Pipe &p) {
  if (p.ours >= 0) close(p.ours);
  if (p.theirs >= 0) close(p.theirs);
  p.ours = -1;
  p.theirs = -1;
}

int64_t do_spawn(void *p) {
  SpawnCall *c = (SpawnCall *)p;

  const char *argv[kMaxArgs + 1];
  int64_t at = 0;
  const char *walk = c->blob;
  for (int64_t i = 0; i < c->count && i < kMaxArgs; i++) {
    argv[at++] = walk;
    walk += strlen(walk) + 1;
  }
  argv[at] = nullptr;

  Pipe in, out, err;
  posix_spawn_file_actions_t actions;
  if (posix_spawn_file_actions_init(&actions) != 0) return -1;

  if (c->pipes) {
    int pair[2];
    if (pipe(pair) != 0) {
      posix_spawn_file_actions_destroy(&actions);
      return -1;
    }
    in.theirs = pair[0]; // the child reads its stdin
    in.ours = pair[1];
    if (pipe(pair) != 0) {
      shut(in);
      posix_spawn_file_actions_destroy(&actions);
      return -1;
    }
    out.ours = pair[0];
    out.theirs = pair[1];
    if (pipe(pair) != 0) {
      shut(in);
      shut(out);
      posix_spawn_file_actions_destroy(&actions);
      return -1;
    }
    err.ours = pair[0];
    err.theirs = pair[1];

    // The child gets its end on 0, 1 and 2, and neither end of ours.
    posix_spawn_file_actions_adddup2(&actions, in.theirs, 0);
    posix_spawn_file_actions_adddup2(&actions, out.theirs, 1);
    posix_spawn_file_actions_adddup2(&actions, err.theirs, 2);
    posix_spawn_file_actions_addclose(&actions, in.ours);
    posix_spawn_file_actions_addclose(&actions, out.ours);
    posix_spawn_file_actions_addclose(&actions, err.ours);
  }

  pid_t child = 0;
  // posix_spawnp, so a bare name is looked up in PATH the way a shell would.
  int failed = posix_spawnp(&child, argv[0], &actions, nullptr,
                            (char *const *)argv, environ);
  posix_spawn_file_actions_destroy(&actions);

  if (failed != 0) {
    shut(in);
    shut(out);
    shut(err);
    return -1;
  }

  // The child has its own copies now; holding ours open would mean a reader
  // that never sees the end of the output.
  if (c->pipes) {
    close(in.theirs);
    close(out.theirs);
    close(err.theirs);
    c->fds[0] = in.ours;
    c->fds[1] = out.ours;
    c->fds[2] = err.ours;
  } else {
    c->fds[0] = -1;
    c->fds[1] = -1;
    c->fds[2] = -1;
  }
  c->pid = (int64_t)child;
  return 0;
}

struct WaitCall {
  int64_t pid;
};

int64_t do_wait(void *p) {
  WaitCall *c = (WaitCall *)p;
  int status = 0;
  while (waitpid((pid_t)c->pid, &status, 0) < 0) {
    if (errno != EINTR) return -1;
  }
  if (WIFEXITED(status)) return WEXITSTATUS(status);
  // The shell's convention, and the only one a single number can carry.
  if (WIFSIGNALED(status)) return 128 + WTERMSIG(status);
  return -1;
}

} // namespace

// `blob` is argv as NUL-separated strings, the program's own name first.
// Answers 0, with the pid and — when pipes were asked for — the parent's end
// of each of the three.
int32_t sword_proc_start(const char *blob, int64_t blob_len, int64_t count,
                         int32_t pipes, int32_t *fds, int64_t *pid) {
  if (count <= 0 || count > kMaxArgs) return -1;
  if (blob_len <= 0) return -1;
  SpawnCall call{blob, count, pipes, {-1, -1, -1}, 0};
  if (sword_offload(do_spawn, &call) != 0) return -1;
  fds[0] = call.fds[0];
  fds[1] = call.fds[1];
  fds[2] = call.fds[2];
  *pid = call.pid;
  return 0;
}

// The exit status, 128 plus the signal when one killed it, -1 when it could
// not be waited for.
int32_t sword_proc_wait(int64_t pid) {
  WaitCall call{pid};
  return (int32_t)sword_offload(do_wait, &call);
}

int32_t sword_proc_kill(int64_t pid, int32_t sig) {
  return kill((pid_t)pid, sig) == 0 ? 0 : -1;
}
}
