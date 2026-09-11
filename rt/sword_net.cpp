#include "sword_net.h"

#include <arpa/inet.h>
#include <errno.h>
#include <netdb.h>
#include <netinet/in.h>
#include <netinet/tcp.h>
#include <stdio.h>
#include <string.h>
#include <sys/socket.h>
#include <unistd.h>

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

int32_t sword_net_dial(const char *host, int64_t host_len, int32_t port) {
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

  addrinfo *found = nullptr;
  if (getaddrinfo(name, service, &hints, &found) != 0) return -1;

  int fd = -1;
  for (addrinfo *at = found; at; at = at->ai_next) {
    fd = socket(at->ai_family, at->ai_socktype, at->ai_protocol);
    if (fd < 0) continue;
    if (connect(fd, at->ai_addr, at->ai_addrlen) == 0) break;
    close(fd);
    fd = -1;
  }
  freeaddrinfo(found);

  if (fd >= 0) {
    int on = 1;
    setsockopt(fd, IPPROTO_TCP, TCP_NODELAY, &on, sizeof(on));
  }
  return fd;
}

int64_t sword_net_read(int32_t fd, void *buf, int64_t len) {
  while (true) {
    ssize_t n = read(fd, buf, (size_t)len);
    if (n >= 0 || errno != EINTR) return n;
  }
}

int64_t sword_net_write(int32_t fd, const void *buf, int64_t len) {
  // Short writes are normal on a socket; the caller wants all or nothing.
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

int32_t sword_net_close(int32_t fd) { return close(fd); }

// A plain close does not wake a thread sitting in accept() on the same
// descriptor; shutting the socket down first does.
int32_t sword_net_stop(int32_t fd) {
  shutdown(fd, SHUT_RDWR);
  return close(fd);
}
}
