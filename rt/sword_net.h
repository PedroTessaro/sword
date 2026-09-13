#pragma once

#include <stdint.h>

// Thin socket layer. It lives in C rather than in Sword because `sockaddr_in`
// is laid out differently on BSD and Linux, and Sword has no conditional
// compilation to tell them apart. Everything above this is written in Sword.
extern "C" {

// Returns a listening socket, or -1. Port 0 asks the OS to choose one.
// The plain form binds loopback; the other takes an address, empty meaning
// every interface, and can share the port with other sockets.
int32_t sword_net_listen(int32_t port, int32_t backlog);
int32_t sword_net_listen_on(const char *host, int64_t host_len, int32_t port,
                            int32_t backlog, int32_t reuse_port);
// Writes the peer's address into `out` and returns its port, or -1.
int32_t sword_net_peer(int32_t fd, char *out, int64_t out_len);
int32_t sword_net_port(int32_t fd);
int32_t sword_net_accept(int32_t fd);
int32_t sword_net_dial(const char *host, int64_t host_len, int32_t port);
// Same, but gives up after `millis`. Zero waits as long as the kernel would.
int32_t sword_net_dial_timeout(const char *host, int64_t host_len, int32_t port,
                               int64_t millis);
int64_t sword_net_read(int32_t fd, void *buf, int64_t len);
int64_t sword_net_write(int32_t fd, const void *buf, int64_t len);
// Milliseconds a single wait may take; zero waits as long as it takes. Applies
// to both directions.
int32_t sword_net_timeout(int32_t fd, int64_t millis);
// An absolute point on the monotonic clock, after which nothing on this socket
// waits any longer. Zero clears it.
int32_t sword_net_deadline(int32_t fd, int64_t at_ns);
// Waits for a socket to be readable or writable, honouring whatever deadline it
// carries: 0 ready, -2 out of time, -1 failed. Exported for the TLS layer, which
// drives the same sockets and has to wait the same way.
int32_t sword_net_await(int32_t fd, int32_t writable);
int32_t sword_net_close(int32_t fd);
// Closing a listener has to wake whoever is blocked in accept on it.
int32_t sword_net_stop(int32_t fd);
}
