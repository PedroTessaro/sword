#include "sword_net.h"
#include "sword_os.h"
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

#include <mutex>
#include <vector>

namespace {

// For the calls that still stop the thread outright — name resolution is the
// one that matters. The scheduler covers the thread while it is gone.
struct Parked {
  Parked() { sword_blocking_enter(); }
  ~Parked() { sword_blocking_exit(); }
};

// Every socket is non-blocking underneath; waiting is the runtime's job rather
// than the kernel's. Inside a task that means putting the task down and letting
// the worker go elsewhere; outside one — the main thread before any scope —
// there is nothing to put down, so the thread waits on poll() instead.
void unblock(int fd) {
  int flags = fcntl(fd, F_GETFL, 0);
  if (flags >= 0) fcntl(fd, F_SETFL, flags | O_NONBLOCK);
}

// Two limits per descriptor, both in monotonic nanoseconds and both optional.
// The timeout bounds one wait; the deadline bounds the whole exchange, which is
// what keeps a client that sends a byte a second from living forever. The
// kernel's own SO_RCVTIMEO cannot do either, because the waiting is the
// runtime's now and not the kernel's.
struct Limits {
  int64_t timeout = 0;  // per wait, relative
  int64_t deadline = 0; // absolute, for everything on this socket
};

std::vector<Limits> limits;
std::mutex limits_lock;

Limits limits_of(int fd) {
  std::lock_guard<std::mutex> held(limits_lock);
  if (fd < 0 || (size_t)fd >= limits.size()) return Limits{};
  return limits[fd];
}

Limits &limits_for(int fd) {
  if ((size_t)fd >= limits.size()) limits.resize((size_t)fd + 64);
  return limits[fd];
}

void forget_limits(int fd) {
  std::lock_guard<std::mutex> held(limits_lock);
  if (fd >= 0 && (size_t)fd < limits.size()) limits[fd] = Limits{};
}

// Whichever runs out first.
int64_t due_at(int fd) {
  Limits l = limits_of(fd);
  int64_t from_timeout = l.timeout > 0 ? sword_time_mono() + l.timeout : 0;
  if (l.deadline == 0) return from_timeout;
  if (from_timeout == 0) return l.deadline;
  return from_timeout < l.deadline ? from_timeout : l.deadline;
}

// Waits for one direction of a descriptor. -2 is the deadline, -1 a failure.
int await(int fd, bool writable) {
  int64_t deadline = due_at(fd);
  if (sword_in_task()) {
    int got = sword_park_fd(fd, writable ? 1 : 0, deadline);
    // -1 only comes back when there was no task to put down, which cannot
    // happen here; anything else is the poller's answer.
    return got;
  }

  // No task to put down: wait on the thread, and tell the scheduler so it can
  // cover for it.
  Parked parked;
  int wait = -1;
  if (deadline > 0) {
    int64_t left = deadline - sword_time_mono();
    if (left <= 0) return -2;
    wait = (int)((left + 999999) / 1000000);
  }
  pollfd watch;
  watch.fd = fd;
  watch.events = writable ? POLLOUT : POLLIN;
  watch.revents = 0;
  int ready = poll(&watch, 1, wait);
  if (ready == 0) return -2;
  if (ready < 0) return errno == EINTR ? 0 : -1;
  return 0;
}

} // namespace

extern "C" {

// `host` is the address to bind, empty meaning every interface. Loopback used to
// be hard-coded here, which meant nothing could be served off the machine.
//
// `reuse_port` lets several sockets — several processes, usually — hold the
// same port and have the kernel spread connections between them. It is how a
// restart happens without dropping anything.
int32_t sword_net_listen_on(const char *host, int64_t host_len, int32_t port,
                            int32_t backlog, int32_t reuse_port) {
  sword_os_ignore_sigpipe();
  int fd = socket(AF_INET, SOCK_STREAM, 0);
  if (fd < 0) return -1;

  int on = 1;
  setsockopt(fd, SOL_SOCKET, SO_REUSEADDR, &on, sizeof(on));
#ifdef SO_REUSEPORT
  if (reuse_port) setsockopt(fd, SOL_SOCKET, SO_REUSEPORT, &on, sizeof(on));
#endif

  sockaddr_in addr;
  memset(&addr, 0, sizeof(addr));
  addr.sin_family = AF_INET;
  addr.sin_port = htons((uint16_t)port);
  if (host_len <= 0) {
    addr.sin_addr.s_addr = htonl(INADDR_ANY);
  } else {
    char name[64];
    if (host_len >= (int64_t)sizeof(name)) {
      close(fd);
      return -1;
    }
    memcpy(name, host, (size_t)host_len);
    name[host_len] = '\0';
    if (inet_pton(AF_INET, name, &addr.sin_addr) != 1) {
      close(fd);
      return -1;
    }
  }

  if (bind(fd, (sockaddr *)&addr, sizeof(addr)) < 0 ||
      listen(fd, backlog) < 0) {
    close(fd);
    return -1;
  }
  unblock(fd);
  return fd;
}

int32_t sword_net_listen(int32_t port, int32_t backlog) {
  const char *loopback = "127.0.0.1";
  return sword_net_listen_on(loopback, 9, port, backlog, 0);
}

// Who is on the other end, written into `out` as dotted quad. Returns the port,
// or -1. A log or a rate limiter needs this and had no way to ask.
int32_t sword_net_peer(int32_t fd, char *out, int64_t out_len) {
  sockaddr_in addr;
  socklen_t len = sizeof(addr);
  if (getpeername(fd, (sockaddr *)&addr, &len) < 0) return -1;
  if (!inet_ntop(AF_INET, &addr.sin_addr, out, (socklen_t)out_len)) return -1;
  return ntohs(addr.sin_port);
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
  while (true) {
    int client = accept(fd, nullptr, nullptr);
    if (client >= 0) {
      int on = 1;
      setsockopt(client, IPPROTO_TCP, TCP_NODELAY, &on, sizeof(on));
      unblock(client);
      forget_limits(client);
      return client;
    }
    if (errno == EINTR) continue; // a signal, not a failure
    if (errno != EAGAIN && errno != EWOULDBLOCK) return -1;
    // Nothing waiting. Put the task down rather than the thread.
    int ready = await(fd, false);
    if (ready == -2) return -2;
    if (ready < 0) return -1;
  }
}

// Connect with a deadline. Going through a non-blocking connect and poll is
// the only way to bound it: the kernel's own connect timeout is over a minute
// and cannot be shortened per socket.
static int connect_within(int fd, const sockaddr *addr, socklen_t len,
                          int64_t millis) {
  unblock(fd);
  forget_limits(fd);
  if (millis > 0) {
    std::lock_guard<std::mutex> held(limits_lock);
    limits_for(fd).timeout = millis * 1000000;
  }

  int result = connect(fd, addr, len);
  if (result == 0) return 0;
  if (errno != EINPROGRESS) return -1;

  int ready = await(fd, true);
  if (ready == -2) {
    errno = ETIMEDOUT;
    return -1;
  }
  if (ready < 0) return -1;

  int failure = 0;
  socklen_t size = sizeof(failure);
  if (getsockopt(fd, SOL_SOCKET, SO_ERROR, &failure, &size) < 0)
    failure = errno;
  errno = failure;
  return failure == 0 ? 0 : -1;
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

  sword_os_ignore_sigpipe();
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
  std::lock_guard<std::mutex> held(limits_lock);
  limits_for(fd).timeout = millis > 0 ? millis * 1000000 : 0;
  return 0;
}

// An absolute point on the monotonic clock, after which nothing on this socket
// waits any longer. Zero clears it.
int32_t sword_net_deadline(int32_t fd, int64_t at_ns) {
  std::lock_guard<std::mutex> held(limits_lock);
  limits_for(fd).deadline = at_ns;
  return 0;
}

// -2 rather than -1 for a timeout, so a caller can tell "nothing arrived in
// time" from "this connection is broken".
int64_t sword_net_read(int32_t fd, void *buf, int64_t len) {
  while (true) {
    ssize_t n = read(fd, buf, (size_t)len);
    if (n >= 0) return n;
    if (errno == EINTR) continue;
    if (errno != EAGAIN && errno != EWOULDBLOCK) return -1;
    int ready = await(fd, false);
    if (ready == -2) return -2;
    if (ready < 0) return -1;
  }
}

int64_t sword_net_write(int32_t fd, const void *buf, int64_t len) {
  // Short writes are normal on a socket; the caller wants all or nothing.
  int64_t sent = 0;
  while (sent < len) {
    ssize_t n = write(fd, (const char *)buf + sent, (size_t)(len - sent));
    if (n < 0) {
      if (errno == EINTR) continue;
      if (errno != EAGAIN && errno != EWOULDBLOCK) return -1;
      int ready = await(fd, true);
      if (ready == -2) return -2;
      if (ready < 0) return -1;
      continue;
    }
    if (n == 0) return -1;
    sent += n;
  }
  return sent;
}

int32_t sword_net_close(int32_t fd) {
  sword_forget_fd(fd);
  forget_limits(fd);
  return close(fd);
}

// A plain close does not wake a thread sitting in accept() on the same
// descriptor; shutting the socket down first does.
int32_t sword_net_stop(int32_t fd) {
  shutdown(fd, SHUT_RDWR);
  sword_forget_fd(fd);
  forget_limits(fd);
  return close(fd);
}
}
