#include "sword_poll.h"
#include "sword_os.h"

#include <atomic>
#include <errno.h>
#include <fcntl.h>
#include <stdio.h>
#include <stdlib.h>
#include <map>
#include <mutex>
#include <string.h>
#include <thread>
#include <unistd.h>
#include <vector>

#if defined(__linux__)
#include <sys/epoll.h>
#define SWORD_EPOLL 1
#else
#include <sys/event.h>
#define SWORD_KQUEUE 1
#endif

namespace {

// Several tasks may wait on the same descriptor and direction: a server with
// more than one acceptor has all of them sitting on one listener. Readiness
// wakes every one of them, and the losers find EAGAIN and come back — which is
// what the kernel means by a ready listener anyway.
struct Waiter {
  void *token = nullptr;
  uint64_t seq = 0; // which registration this was, so a stale timer is ignored
};

struct Slot {
  std::vector<Waiter> waiting;
};

struct Poller {
  int handle = -1;     // the kqueue or epoll descriptor
  int nudge[2] = {-1, -1}; // a pipe, to cut a wait short when a deadline moves
  std::thread thread;
  std::atomic<bool> stopping{false};
  sword_wake_fn wake = nullptr;

  std::mutex lock;
  // Indexed by descriptor, two slots each. Descriptors are small and dense, so
  // a vector beats a hash for the lookup that happens on every event.
  std::vector<Slot> slots;
  std::multimap<int64_t, std::pair<int, uint64_t>> timers; // deadline -> slot
  uint64_t next_seq = 1;
};

Poller &poller();

size_t slot_of(int fd, int writable) {
  return (size_t)fd * 2 + (writable ? 1 : 0);
}

void make_room(Poller &p, size_t slot) {
  if (p.slots.size() <= slot) p.slots.resize(slot + 64);
}

// Hands each token back exactly once: whoever takes it out of the slot owns the
// wake. A descriptor that becomes ready and times out in the same breath
// therefore wakes its tasks once, with whichever answer arrived first.
void deliver(Poller &p, size_t slot, int result) {
  std::vector<Waiter> woken;
  {
    std::lock_guard<std::mutex> held(p.lock);
    if (slot >= p.slots.size() || p.slots[slot].waiting.empty()) return;
    woken.swap(p.slots[slot].waiting);
  }
  for (const Waiter &w : woken) p.wake(w.token, result);
}

// One waiter, for a deadline that belongs to it alone.
void deliver_one(Poller &p, size_t slot, uint64_t seq, int result) {
  void *token = nullptr;
  {
    std::lock_guard<std::mutex> held(p.lock);
    if (slot >= p.slots.size()) return;
    auto &waiting = p.slots[slot].waiting;
    for (size_t i = 0; i < waiting.size(); i++) {
      if (waiting[i].seq != seq) continue;
      token = waiting[i].token;
      waiting.erase(waiting.begin() + (long)i);
      break;
    }
  }
  if (token) p.wake(token, result);
}

void arm(Poller &p, int fd, int writable) {
#ifdef SWORD_KQUEUE
  struct kevent change;
  EV_SET(&change, fd, writable ? EVFILT_WRITE : EVFILT_READ,
         EV_ADD | EV_ONESHOT, 0, 0, nullptr);
  kevent(p.handle, &change, 1, nullptr, 0, nullptr);
#else
  // epoll is per descriptor rather than per filter, so both directions share
  // one registration and the events are rebuilt from what is still waiting.
  std::lock_guard<std::mutex> held(p.lock);
  epoll_event ev;
  memset(&ev, 0, sizeof(ev));
  ev.data.fd = fd;
  ev.events = EPOLLONESHOT;
  size_t base = slot_of(fd, 0);
  if (base < p.slots.size() && !p.slots[base].waiting.empty())
    ev.events |= EPOLLIN;
  if (base + 1 < p.slots.size() && !p.slots[base + 1].waiting.empty())
    ev.events |= EPOLLOUT;
  if (epoll_ctl(p.handle, EPOLL_CTL_MOD, fd, &ev) != 0)
    epoll_ctl(p.handle, EPOLL_CTL_ADD, fd, &ev);
#endif
}

void disarm(Poller &p, int fd) {
#ifdef SWORD_KQUEUE
  struct kevent change[2];
  EV_SET(&change[0], fd, EVFILT_READ, EV_DELETE, 0, 0, nullptr);
  EV_SET(&change[1], fd, EVFILT_WRITE, EV_DELETE, 0, 0, nullptr);
  kevent(p.handle, change, 2, nullptr, 0, nullptr);
#else
  epoll_ctl(p.handle, EPOLL_CTL_DEL, fd, nullptr);
#endif
}

void nudge(Poller &p) {
  char one = 1;
  ssize_t ignored = write(p.nudge[1], &one, 1);
  (void)ignored;
}

// Milliseconds until the nearest deadline, or -1 when there is none. Capped so
// that a poller with nothing to do still notices a stop.
int next_timeout(Poller &p) {
  std::lock_guard<std::mutex> held(p.lock);
  if (p.timers.empty()) return 200;
  int64_t now = sword_time_mono();
  int64_t soonest = p.timers.begin()->first;
  if (soonest <= now) return 0;
  int64_t millis = (soonest - now + 999999) / 1000000;
  return millis > 200 ? 200 : (int)millis;
}

void expire(Poller &p) {
  int64_t now = sword_time_mono();
  std::vector<std::pair<size_t, uint64_t>> due;
  {
    std::lock_guard<std::mutex> held(p.lock);
    auto it = p.timers.begin();
    while (it != p.timers.end() && it->first <= now) {
      due.push_back({(size_t)it->second.first, it->second.second});
      it = p.timers.erase(it);
    }
  }
  // A registration that has already been answered is simply not there any
  // more, so the sequence number is all the checking this needs.
  for (const auto &entry : due)
    deliver_one(p, entry.first, entry.second, SWORD_POLL_TIMEOUT);
}

void drain_nudge(Poller &p) {
  char buffer[64];
  while (read(p.nudge[0], buffer, sizeof(buffer)) > 0) {
  }
}

void loop() {
  Poller &p = poller();
#ifdef SWORD_KQUEUE
  std::vector<struct kevent> events(256);
#else
  std::vector<epoll_event> events(256);
#endif

  while (!p.stopping.load(std::memory_order_acquire)) {
    int timeout = next_timeout(p);

#ifdef SWORD_KQUEUE
    timespec wait;
    wait.tv_sec = timeout / 1000;
    wait.tv_nsec = (long)(timeout % 1000) * 1000000;
    int count = kevent(p.handle, nullptr, 0, events.data(),
                       (int)events.size(), &wait);
#else
    int count = epoll_wait(p.handle, events.data(), (int)events.size(),
                           timeout);
#endif
    if (count < 0) {
      if (errno == EINTR) continue;
      break;
    }

    for (int i = 0; i < count; i++) {
#ifdef SWORD_KQUEUE
      int fd = (int)events[i].ident;
      bool writable = events[i].filter == EVFILT_WRITE;
      if (fd == p.nudge[0]) {
        drain_nudge(p);
        arm(p, p.nudge[0], 0);
        continue;
      }
      deliver(p, slot_of(fd, writable), SWORD_POLL_READY);
#else
      int fd = events[i].data.fd;
      if (fd == p.nudge[0]) {
        drain_nudge(p);
        arm(p, p.nudge[0], 0);
        continue;
      }
      uint32_t got = events[i].events;
      // A hangup or an error wakes both directions: whoever is waiting needs
      // to find out from the syscall itself what went wrong.
      bool broken = (got & (EPOLLERR | EPOLLHUP)) != 0;
      if ((got & EPOLLIN) || broken) deliver(p, slot_of(fd, 0), SWORD_POLL_READY);
      if ((got & EPOLLOUT) || broken) deliver(p, slot_of(fd, 1), SWORD_POLL_READY);
#endif
    }
    expire(p);
  }
}

Poller &poller() {
  static Poller *instance = new Poller();
  return *instance;
}

std::once_flag started;

} // namespace

extern "C" {

void sword_poll_start(sword_wake_fn wake) {
  Poller &p = poller();
  std::call_once(started, [&p, wake] {
    p.wake = wake;
#ifdef SWORD_KQUEUE
    p.handle = kqueue();
#else
    p.handle = epoll_create1(0);
#endif
    if (p.handle < 0) {
      fputs("sword: cannot create the poller\n", stderr);
      abort();
    }
    if (pipe(p.nudge) != 0) {
      fputs("sword: cannot create the poller's pipe\n", stderr);
      abort();
    }
    // Non-blocking, so draining it never parks the poller itself.
    for (int end = 0; end < 2; end++) {
      int flags = fcntl(p.nudge[end], F_GETFL, 0);
      fcntl(p.nudge[end], F_SETFL, flags | O_NONBLOCK);
    }
    arm(p, p.nudge[0], 0);
    p.thread = std::thread(loop);
  });
}

void sword_poll_wait(int fd, int writable, void *token, int64_t deadline_ns) {
  Poller &p = poller();
  size_t slot = slot_of(fd, writable);
  uint64_t seq;
  {
    std::lock_guard<std::mutex> held(p.lock);
    make_room(p, slot);
    seq = p.next_seq++;
    p.slots[slot].waiting.push_back(Waiter{token, seq});
    if (deadline_ns > 0)
      p.timers.insert({deadline_ns, {(int)slot, seq}});
  }
  arm(p, fd, writable);
  // A deadline nearer than the one the poller is already waiting on has to cut
  // that wait short, or it would be noticed late.
  if (deadline_ns > 0) nudge(p);
}

// Whoever is waiting has to be told, not dropped: a descriptor is forgotten
// when it is about to be closed, and a task still parked on it would never be
// woken by anything else.
void sword_poll_forget(int fd) {
  Poller &p = poller();
  if (p.handle < 0) return;
  disarm(p, fd);
  deliver(p, slot_of(fd, 0), SWORD_POLL_FAILED);
  deliver(p, slot_of(fd, 1), SWORD_POLL_FAILED);
}

void sword_poll_stop(void) {
  Poller &p = poller();
  if (p.handle < 0) return;
  p.stopping.store(true, std::memory_order_release);
  nudge(p);
  if (p.thread.joinable()) p.thread.join();
}
}
