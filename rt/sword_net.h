#pragma once

#include <stdint.h>

// Thin socket layer. It lives in C rather than in Sword because `sockaddr_in`
// is laid out differently on BSD and Linux, and Sword has no conditional
// compilation to tell them apart. Everything above this is written in Sword.
extern "C" {

// Returns a listening socket, or -1. Port 0 asks the OS to choose one.
int32_t sword_net_listen(int32_t port, int32_t backlog);
int32_t sword_net_port(int32_t fd);
int32_t sword_net_accept(int32_t fd);
int32_t sword_net_dial(const char *host, int64_t host_len, int32_t port);
int64_t sword_net_read(int32_t fd, void *buf, int64_t len);
int64_t sword_net_write(int32_t fd, const void *buf, int64_t len);
// Milliseconds; zero means wait forever. Applies to both directions.
int32_t sword_net_timeout(int32_t fd, int64_t millis);
int32_t sword_net_close(int32_t fd);
// Closing a listener has to wake whoever is blocked in accept on it.
int32_t sword_net_stop(int32_t fd);
}
