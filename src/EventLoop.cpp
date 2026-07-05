#include "EventLoop.h"

#include "Commands.h"

#include <arpa/inet.h>
#include <netinet/in.h>
#include <sys/epoll.h>
#include <sys/socket.h>
#include <unistd.h>

#include <cerrno>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <stdexcept>

namespace {

[[noreturn]] void die(const char* what) {
    std::perror(what);
    std::exit(1);
}

}  // namespace

EventLoop::EventLoop(std::uint16_t port) {
    // Non-blocking listener. SOCK_NONBLOCK/SOCK_CLOEXEC at creation time
    // avoids separate fcntl round-trips (and a race on the flags).
    listen_fd_ = ::socket(AF_INET, SOCK_STREAM | SOCK_NONBLOCK | SOCK_CLOEXEC, 0);
    if (listen_fd_ < 0) die("socket");

    int yes = 1;
    ::setsockopt(listen_fd_, SOL_SOCKET, SO_REUSEADDR, &yes, sizeof yes);

    sockaddr_in addr{};
    addr.sin_family = AF_INET;
    addr.sin_addr.s_addr = htonl(INADDR_ANY);
    addr.sin_port = htons(port);
    if (::bind(listen_fd_, reinterpret_cast<sockaddr*>(&addr), sizeof addr) < 0)
        die("bind");
    if (::listen(listen_fd_, SOMAXCONN) < 0) die("listen");

    epfd_ = ::epoll_create1(EPOLL_CLOEXEC);
    if (epfd_ < 0) die("epoll_create1");

    // Listener: level-triggered EPOLLIN, data.ptr = nullptr marks it in
    // dispatch. (We accept in a loop until EAGAIN anyway, so LT vs ET is
    // immaterial here; LT is the forgiving choice.)
    epoll_event ev{};
    ev.events = EPOLLIN;
    ev.data.ptr = nullptr;
    if (::epoll_ctl(epfd_, EPOLL_CTL_ADD, listen_fd_, &ev) < 0)
        die("epoll_ctl(listener)");

    std::printf("redis-lite listening on port %u\n", port);
}

EventLoop::~EventLoop() {
    // Connections close themselves via ~Connection (RAII).
    if (listen_fd_ >= 0) ::close(listen_fd_);
    if (epfd_ >= 0) ::close(epfd_);
}

void EventLoop::run() {
    constexpr int kMaxEvents = 1024;
    epoll_event events[kMaxEvents];

    for (;;) {
        // TODO(phase 5): replace -1 with next_expiry_ms() from the store's
        // TTL heap, then reap due keys on timeout wakeups. That single
        // argument is the entire "timers" mechanism.
        int n = ::epoll_wait(epfd_, events, kMaxEvents, -1);
        if (n < 0) {
            if (errno == EINTR) continue;  // signal: benign, retry
            die("epoll_wait");
        }

        for (int i = 0; i < n; ++i) {
            if (events[i].data.ptr == nullptr) {
                accept_new();
                continue;
            }

            auto* c = static_cast<Connection*>(events[i].data.ptr);
            std::uint32_t e = events[i].events;

            // Error/hangup: mark ready for both, mirroring ae_epoll.c --
            // the next read()/write() returns the error and we clean up
            // through the normal path instead of special-casing here.
            if (e & (EPOLLERR | EPOLLHUP)) c->want_close = true;

            if (!c->want_close && (e & EPOLLIN)) handle_readable(c);
            if (!c->want_close && (e & EPOLLOUT)) handle_writable(c);

            if (c->want_close) close_conn(c);
            // Each fd appears at most once per epoll_wait batch, so closing
            // here can't invalidate a later event in the same batch.
        }
    }
}

void EventLoop::accept_new() {
    // Drain the accept queue completely (required under ET; harmless
    // under LT).
    for (;;) {
        int fd = ::accept4(listen_fd_, nullptr, nullptr,
                           SOCK_NONBLOCK | SOCK_CLOEXEC);
        if (fd < 0) {
            if (errno == EAGAIN || errno == EWOULDBLOCK) return;  // drained
            if (errno == EINTR) continue;
            std::perror("accept4");  // transient (EMFILE etc.): keep serving
            return;
        }

        auto conn = std::make_unique<Connection>(fd);

        epoll_event ev{};
        ev.events = EPOLLIN | EPOLLET;  // edge-triggered from birth
        ev.data.ptr = conn.get();
        if (::epoll_ctl(epfd_, EPOLL_CTL_ADD, fd, &ev) < 0) {
            std::perror("epoll_ctl(add conn)");
            continue;  // conn destructs, fd closes
        }

        conns_.emplace(fd, std::move(conn));
    }
}

void EventLoop::handle_readable(Connection* c) {
    // ET contract: read until EAGAIN. Stop early and the kernel will not
    // re-notify for the bytes left behind -- the connection hangs.
    char buf[64 * 1024];
    for (;;) {
        ssize_t r = ::read(c->fd(), buf, sizeof buf);
        if (r > 0) {
            c->in.append(buf, static_cast<std::size_t>(r));
            // TODO(hardening): cap c->in.size(); a client streaming an
            // unbounded never-complete command is a memory DoS. Past the
            // cap: reply -ERR and want_close.
            continue;
        }
        if (r == 0) {                 // orderly EOF from peer
            c->want_close = true;
            break;
        }
        if (errno == EAGAIN || errno == EWOULDBLOCK) break;  // drained
        if (errno == EINTR) continue;
        c->want_close = true;         // real error (ECONNRESET etc.)
        break;
    }

    process_input(c);
    try_flush(c);
}

void EventLoop::process_input(Connection* c) {
    // Loop until NeedMore: drains every complete command in the buffer,
    // which is exactly what makes pipelining work.
    for (;;) {
        std::vector<std::string> args;
        ParseStatus st = c->parser.parse(c->in, args);
        if (st == ParseStatus::NeedMore) break;
        if (st == ParseStatus::Error) {
            reply_error(c->out, "Protocol error");
            c->want_close = true;  // try_flush gets one shot to deliver it
            break;
        }
        execute_command(args, store_, c->out);
    }
}

void EventLoop::handle_writable(Connection* c) { try_flush(c); }

void EventLoop::try_flush(Connection* c) {
    // Immediate-write fast path + ET drain: write until empty or EAGAIN.
    while (!c->out.empty()) {
        auto view = c->out.readable();
        ssize_t w = ::write(c->fd(), view.data(), view.size());
        if (w > 0) {
            c->out.consume(static_cast<std::size_t>(w));
            continue;
        }
        if (w < 0 && (errno == EAGAIN || errno == EWOULDBLOCK)) break;
        if (w < 0 && errno == EINTR) continue;
        c->want_close = true;  // real error
        return;
    }
    update_epollout(c);
}

void EventLoop::update_epollout(Connection* c) {
    // Arm EPOLLOUT only while output is pending; disarm the moment it
    // drains. Leaving it armed on an idle socket = epoll_wait returns
    // instantly forever = 100% CPU busy loop.
    bool need = !c->out.empty();
    if (need == c->epollout_armed) return;  // no state change, no syscall

    epoll_event ev{};
    ev.events = EPOLLIN | EPOLLET | (need ? EPOLLOUT : 0u);
    ev.data.ptr = c;
    if (::epoll_ctl(epfd_, EPOLL_CTL_MOD, c->fd(), &ev) < 0) {
        std::perror("epoll_ctl(mod)");
        c->want_close = true;
        return;
    }
    c->epollout_armed = need;
}

void EventLoop::close_conn(Connection* c) {
    // Erasing drops the unique_ptr; ~Connection closes the fd, which also
    // removes it from the epoll interest list.
    conns_.erase(c->fd());
}