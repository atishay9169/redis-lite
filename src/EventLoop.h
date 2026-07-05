#pragma once

#include <cstdint>
#include <memory>
#include <unordered_map>

#include "Connection.h"
#include "Store.h"

// Single-threaded epoll reactor.
//
// One thread, one epoll instance, non-blocking sockets, edge-triggered
// connections. No locks anywhere -- correctness by construction.
//
// Event dispatch: epoll_event.data.ptr is nullptr for the listener and a
// Connection* for client sockets. conns_ (fd -> unique_ptr) exists purely
// for ownership; dispatch never looks anything up.
class EventLoop {
public:
    explicit EventLoop(std::uint16_t port);
    ~EventLoop();

    EventLoop(const EventLoop&) = delete;
    EventLoop& operator=(const EventLoop&) = delete;

    // Runs forever (until a fatal error).
    void run();

private:
    void accept_new();
    void handle_readable(Connection* c);
    void handle_writable(Connection* c);

    // PHASE 1: echoes c->in into c->out.
    // PHASE 2+: replace body with parse-loop -> execute -> serialize reply.
    void process_input(Connection* c);

    // write() until drained or EAGAIN, then arm/disarm EPOLLOUT to match.
    void try_flush(Connection* c);
    void update_epollout(Connection* c);

    void close_conn(Connection* c);

    int epfd_ = -1;
    int listen_fd_ = -1;
    Store store_;
    std::unordered_map<int, std::unique_ptr<Connection>> conns_;
};
