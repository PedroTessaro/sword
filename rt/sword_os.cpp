#include "sword_os.h"
#include "sword_rt.h"

#include <atomic>
#include <errno.h>
#include <fcntl.h>
#include <poll.h>
#include <signal.h>
#include <stdlib.h>
#include <sys/resource.h>
#include <string.h>
#include <time.h>
#include <unistd.h>

namespace {

int32_t saved_argc = 0;
char **saved_argv = nullptr;

// A signal arrives on whatever thread the kernel likes, in a context where
// almost nothing is safe to call. So the handler does the one thing that is —
// writes a byte down a pipe — and the waiting is ordinary polled reading on the
// other end, which the scheduler already knows how to put a task down for.
int signal_pipe[2] = {-1, -1};

// Which signals were asked for, so that giving up on them can put each one
// back the way it was. Anything past 63 is not tracked and not catchable here.
std::atomic<uint64_t> caught{0};
// Set once and never cleared: a program that has said it is done watching for
// signals gets the same answer every time it asks again.
std::atomic<bool> stopped{false};

void on_signal(int sig) {
  unsigned char which = (unsigned char)sig;
  ssize_t ignored = write(signal_pipe[1], &which, 1);
  (void)ignored;
}

bool open_signal_pipe() {
  if (signal_pipe[0] >= 0) return true;
  if (pipe(signal_pipe) != 0) return false;
  for (int end = 0; end < 2; end++) {
    int flags = fcntl(signal_pipe[end], F_GETFL, 0);
    fcntl(signal_pipe[end], F_SETFL, flags | O_NONBLOCK);
  }
  return true;
}

} // namespace

extern "C" {

void sword_os_set_args(int32_t argc, char **argv) {
  saved_argc = argc;
  saved_argv = argv;
}

int64_t sword_os_argc(void) { return saved_argc; }

const char *sword_os_arg(int64_t i, int64_t *len) {
  if (i < 0 || i >= saved_argc || !saved_argv) return nullptr;
  const char *text = saved_argv[i];
  *len = (int64_t)strlen(text);
  return text;
}

const char *sword_os_env(const char *name, int64_t name_len, int64_t *len) {
  char key[256];
  if (name_len <= 0 || name_len >= (int64_t)sizeof(key)) return nullptr;
  memcpy(key, name, (size_t)name_len);
  key[name_len] = '\0';

  const char *value = getenv(key);
  if (!value) return nullptr;
  *len = (int64_t)strlen(value);
  return value;
}

void sword_os_exit(int32_t code) { exit(code); }

// Writing to a socket the other end has closed raises SIGPIPE, and the default
// for SIGPIPE is to kill the process. No server wants that: the write should
// fail and be handled like any other failure. Called once, from the first
// listen or dial.
void sword_os_ignore_sigpipe(void) {
  static bool done = false;
  if (done) return;
  done = true;
  signal(SIGPIPE, SIG_IGN);
}

// Starts catching a signal. Until this is called the default stands, so a
// program that asks for nothing behaves as it always did.
int32_t sword_os_catch(int32_t sig) {
  // Asking again after the program has given up on signals is a mistake, not a
  // way back: the waits are already answering, and nothing would read what the
  // handler wrote.
  if (stopped.load(std::memory_order_acquire)) return -1;
  if (sig < 0 || sig >= 64) return -1;
  if (!open_signal_pipe()) return -1;
  struct sigaction action;
  memset(&action, 0, sizeof(action));
  action.sa_handler = on_signal;
  sigemptyset(&action.sa_mask);
  action.sa_flags = SA_RESTART;
  if (sigaction(sig, &action, nullptr) != 0) return -1;
  caught.fetch_or((uint64_t)1 << sig, std::memory_order_acq_rel);
  return 0;
}

// Ends every wait, now and later. The byte is what wakes whoever is parked on
// the pipe, and it is deliberately never read: the descriptor stays readable,
// so a task that parks afterwards comes straight back out and is told the same
// thing. Each signal goes back to doing what it would have done — a program
// that has stopped listening should not be one that swallows a SIGTERM.
void sword_os_stop_signals(void) {
  if (stopped.exchange(true, std::memory_order_acq_rel)) return;
  uint64_t asked = caught.load(std::memory_order_acquire);
  for (int sig = 0; sig < 64; sig++)
    if (asked & ((uint64_t)1 << sig)) signal(sig, SIG_DFL);
  if (signal_pipe[1] >= 0) {
    unsigned char wake = 0;
    ssize_t ignored = write(signal_pipe[1], &wake, 1);
    (void)ignored;
  }
}

// The next signal that was asked for, waiting without holding a thread. -1 when
// nothing was ever asked for.
int32_t sword_os_wait_signal(void) {
  if (signal_pipe[0] < 0) return -1;
  while (true) {
    if (stopped.load(std::memory_order_acquire)) return -1;
    unsigned char which = 0;
    ssize_t n = read(signal_pipe[0], &which, 1);
    if (n == 1) return (int32_t)which;
    if (n == 0) return -1;
    if (errno == EINTR) continue;
    if (errno != EAGAIN && errno != EWOULDBLOCK) return -1;

    if (sword_in_task()) {
      if (sword_park_fd(signal_pipe[0], 0, 0) < 0) return -1;
      continue;
    }
    // Outside a task there is nothing to put down, so the thread waits.
    sword_blocking_enter();
    pollfd watch;
    watch.fd = signal_pipe[0];
    watch.events = POLLIN;
    watch.revents = 0;
    int ready = poll(&watch, 1, -1);
    sword_blocking_exit();
    if (ready < 0 && errno != EINTR) return -1;
  }
}

// How many descriptors this process may have open, and raising it as far as the
// hard limit allows. A server that has not done this stops at whatever the
// shell handed it, which on a Mac is 256.
int64_t sword_os_max_files(void) {
  struct rlimit limit;
  if (getrlimit(RLIMIT_NOFILE, &limit) != 0) return -1;
  return (int64_t)limit.rlim_cur;
}

int64_t sword_os_raise_max_files(int64_t want) {
  struct rlimit limit;
  if (getrlimit(RLIMIT_NOFILE, &limit) != 0) return -1;
  rlim_t target = want > 0 ? (rlim_t)want : limit.rlim_max;
  if (limit.rlim_max != RLIM_INFINITY && target > limit.rlim_max)
    target = limit.rlim_max;
  if (target > limit.rlim_cur) {
    limit.rlim_cur = target;
    if (setrlimit(RLIMIT_NOFILE, &limit) != 0) return -1;
  }
  return (int64_t)limit.rlim_cur;
}

int64_t sword_os_pid(void) { return (int64_t)getpid(); }

int32_t sword_os_kill(int64_t pid, int32_t sig) {
  return kill((pid_t)pid, sig) == 0 ? 0 : -1;
}

// Null when it cannot be read, which the caller turns into an optional.
const char *sword_os_hostname(int64_t *len) {
  static char name[256];
  if (gethostname(name, sizeof(name)) != 0) return nullptr;
  name[sizeof(name) - 1] = '\0';
  *len = (int64_t)strlen(name);
  return name;
}

// How many cores the scheduler saw. Useful for sizing a pool of anything else.
int64_t sword_os_cpus(void) {
  long n = sysconf(_SC_NPROCESSORS_ONLN);
  return n > 0 ? (int64_t)n : 1;
}

// clock_gettime rather than gettimeofday: nanosecond resolution, and a
// monotonic clock that a system time adjustment cannot drag backwards.
int64_t sword_time_mono(void) {
  timespec now;
  clock_gettime(CLOCK_MONOTONIC, &now);
  return (int64_t)now.tv_sec * 1000000000 + now.tv_nsec;
}

int64_t sword_time_unix(void) {
  timespec now;
  clock_gettime(CLOCK_REALTIME, &now);
  return (int64_t)now.tv_sec * 1000000000 + now.tv_nsec;
}

void sword_time_sleep(int64_t nanos) {
  if (nanos <= 0) return;
  // Inside a task this costs a timer, not a thread: a program that sleeps in a
  // hundred tasks at once still runs on the workers it had.
  if (sword_park_timer(sword_time_mono() + nanos) == 0) return;

  timespec want;
  want.tv_sec = (time_t)(nanos / 1000000000);
  want.tv_nsec = (long)(nanos % 1000000000);

  sword_blocking_enter();
  timespec left;
  // A signal cuts the sleep short and hands back what is left of it.
  while (nanosleep(&want, &left) != 0 && errno == EINTR) want = left;
  sword_blocking_exit();
}
}
