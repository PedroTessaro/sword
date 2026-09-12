#pragma once

#include <stdint.h>

// What a program needs from the operating system that Sword cannot express on
// its own: the argument vector it was started with, the environment, and a
// clock. Strings come back as a pointer plus a length written through `len`,
// because a C function cannot return a Sword string by value.
extern "C" {

// Called once from the generated entry point, before main runs.
void sword_os_set_args(int32_t argc, char **argv);

int64_t sword_os_argc(void);
// Null when `i` is past the end.
const char *sword_os_arg(int64_t i, int64_t *len);
// Null when the variable is not set. An empty variable is set, not absent.
const char *sword_os_env(const char *name, int64_t name_len, int64_t *len);
void sword_os_exit(int32_t code);

// Nanoseconds on a clock that only moves forward, which is the one to measure
// with: the wall clock can jump backwards.
int64_t sword_time_mono(void);
// Nanoseconds since the Unix epoch, for a timestamp rather than a duration.
int64_t sword_time_unix(void);
void sword_time_sleep(int64_t nanos);
}
