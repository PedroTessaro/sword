#include "sword_net.h"
#include "sword_rt.h"

#include <arpa/inet.h>
#include <errno.h>
#include <fcntl.h>
#include <netdb.h>
#include <netinet/in.h>
#include <netinet/tcp.h>
#include <poll.h>
#include <stdio.h>
#include <string.h>
#include <sys/socket.h>
#include <sys/time.h>
#include <unistd.h>

namespace {

// Everything below parks the thread in the kernel. The scheduler has to know,
// or a handful of quiet sockets would use up the whole pool.
struct Parked {
  Parked() { sword_blocking_enter(); }
  ~Parked() { sword_blocking_exit(); }
};

} // namespace

extern "C" {

int32_t sword_net_listen(int32_t port, int32_t backlog) {
  int fd = socket(AF_INET, SOCK_STREAM, 0);
  if (fd < 0) return -1;

  int on = 1;
  setsockopt(fd, SOL_SOCKET, SO_REUSEADDR, &on, sizeof(on));

  sockaddr_in addr;
  memset(&addr, 0, sizeof(addr));
  addr.sin_family = AF_INET;
  addr.sin_addr.s_addr = htonl(INADDR_LOOPBACK);
  addr.sin_port = htons((uint16_t)port);

  if (bind(fd, (sockaddr *)&addr, sizeof(addr)) < 0 ||
      listen(fd, backlog) < 0) {
    close(fd);
    return -1;
  }
  return fd;
}

// Needed because a test can ask for port 0 and then has to find out which one
// it actually got.
int32_t sword_net_port(int32_t fd) {
  sockaddr_in addr;
  socklen_t len = sizeof(addr);
  if (getsockname(fd, (sockaddr *)&addr, &len) < 0) return -1;
  return ntohs(addr.sin_port);
}

int32_t sword_net_accept(int32_t fd) {
  Parked parked;
  while (true) {
    int client = accept(fd, nullptr, nullptr);
    if (client >= 0) {
      int on = 1;
      setsockopt(client, IPPROTO_TCP, TCP_NODELAY, &on, sizeof(on));
      return client;
    }
    if (errno == EINTR) continue; // a signal, not a failure
    return -1;
  }
}

// Connect with a deadline. Going through a non-blocking connect and poll is
// the only way to bound it: the kernel's own connect timeout is over a minute
// and cannot be shortened per socket.
static int connect_within(int fd, const sockaddr *addr, socklen_t len,
                          int64_t millis) {
  if (millis <= 0) return connect(fd, addr, len);

  int flags = fcntl(fd, F_GETFL, 0);
  if (flags < 0 || fcntl(fd, F_SETFL, flags | O_NONBLOCK) < 0) return -1;

  int result = connect(fd, addr, len);
  if (result < 0 && errno == EINPROGRESS) {
    pollfd watch;
    watch.fd = fd;
    watch.events = POLLOUT;
    watch.revents = 0;
    int ready = poll(&watch, 1, (int)millis);
    if (ready == 0) {
      fcntl(fd, F_SETFL, flags);
      errno = ETIMEDOUT;
      return -1;
    }
    int failure = 0;
    socklen_t size = sizeof(failure);
    if (ready < 0 || getsockopt(fd, SOL_SOCKET, SO_ERROR, &failure, &size) < 0)
      failure = errno;
    result = failure == 0 ? 0 : -1;
    errno = failure;
  }

  fcntl(fd, F_SETFL, flags);
  return result;
}

// Zero waits as long as the kernel would. -2 separates "took too long" from
// "could not connect at all", the same way a read does.
int32_t sword_net_dial_timeout(const char *host, int64_t host_len, int32_t port,
                               int64_t millis) {
  char name[256];
  if (host_len <= 0 || host_len >= (int64_t)sizeof(name)) return -1;
  memcpy(name, host, (size_t)host_len);
  name[host_len] = '\0';

  addrinfo hints;
  memset(&hints, 0, sizeof(hints));
  hints.ai_family = AF_INET;
  hints.ai_socktype = SOCK_STREAM;

  char service[16];
  snprintf(service, sizeof(service), "%d", port);

  Parked parked; // name resolution blocks too, often for longer than connect
  addrinfo *found = nullptr;
  if (getaddrinfo(name, service, &hints, &found) != 0) return -1;

  int fd = -1;
  bool timed_out = false;
  for (addrinfo *at = found; at; at = at->ai_next) {
    fd = socket(at->ai_family, at->ai_socktype, at->ai_protocol);
    if (fd < 0) continue;
    if (connect_within(fd, at->ai_addr, at->ai_addrlen, millis) == 0) break;
    timed_out = errno == ETIMEDOUT;
    close(fd);
    fd = -1;
  }
  freeaddrinfo(found);

  if (fd < 0) return timed_out ? -2 : -1;
  int on = 1;
  setsockopt(fd, IPPROTO_TCP, TCP_NODELAY, &on, sizeof(on));
  return fd;
}

int32_t sword_net_dial(const char *host, int64_t host_len, int32_t port) {
  return sword_net_dial_timeout(host, host_len, port, 0);
}

int32_t sword_net_timeout(int32_t fd, int64_t millis) {
  timeval tv;
  tv.tv_sec = (time_t)(millis / 1000);
  tv.tv_usec = (suseconds_t)((millis % 1000) * 1000);
  if (setsockopt(fd, SOL_SOCKET, SO_RCVTIMEO, &tv, sizeof(tv)) < 0) return -1;
  if (setsockopt(fd, SOL_SOCKET, SO_SNDTIMEO, &tv, sizeof(tv)) < 0) return -1;
  return 0;
}

// -2 rather than -1 for a timeout, so a caller can tell "nothing arrived in
// time" from "this connection is broken".
int64_t sword_net_read(int32_t fd, void *buf, int64_t len) {
  Parked parked;
  while (true) {
    ssize_t n = read(fd, buf, (size_t)len);
    if (n >= 0) return n;
    if (errno == EINTR) continue;
    if (errno == EAGAIN || errno == EWOULDBLOCK) return -2;
    return -1;
  }
}

int64_t sword_net_write(int32_t fd, const void *buf, int64_t len) {
  // Short writes are normal on a socket; the caller wants all or nothing.
  Parked parked;
  int64_t sent = 0;
  while (sent < len) {
    ssize_t n = write(fd, (const char *)buf + sent, (size_t)(len - sent));
    if (n < 0) {
      if (errno == EINTR) continue;
      if (errno == EAGAIN || errno == EWOULDBLOCK) return -2;
      return -1;
    }
    if (n == 0) return -1;
    sent += n;
  }
  return sent;
}

int32_t sword_net_close(int32_t fd) { return close(fd); }

// A plain close does not wake a thread sitting in accept() on the same
// descriptor; shutting the socket down first does.
int32_t sword_net_stop(int32_t fd) {
  shutdown(fd, SHUT_RDWR);
  return close(fd);
}
}
