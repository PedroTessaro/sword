#include "sword_os.h"
#include "sword_rt.h"

#include <errno.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>

namespace {

int32_t saved_argc = 0;
char **saved_argv = nullptr;

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
