#pragma once

#include <unistd.h>

#include "Buffer.h"
#include "RespParser.h"

// Per-connection state, owned by the event loop.
//
// RAII: the destructor closes the fd. Closing an fd also removes it from
// every epoll interest list it belongs to (as long as no duplicates of the
// fd exist), so there is no separate deregistration step to forget.
//
// A raw pointer to this object is stored in epoll_event.data.ptr, giving
// O(1) access to buffers/state on each event and avoiding the fd-reuse
// hazard of keying a map by fd.
class Connection {
public:
    explicit Connection(int fd) : fd_(fd) {}
    ~Connection() {
        if (fd_ >= 0) ::close(fd_);
    }

    Connection(const Connection&) = delete;
    Connection& operator=(const Connection&) = delete;
    Connection(Connection&&) = delete;
    Connection& operator=(Connection&&) = delete;

    int fd() const { return fd_; }

    Buffer in;    // bytes read from the socket, awaiting parsing
    Buffer out;   // reply bytes awaiting write to the socket

    bool want_close = false;     // set on EOF / fatal error; loop reaps it
    bool epollout_armed = false; // are we currently registered for EPOLLOUT?

    RespParser parser;  // per-connection: each client resumes its own state

private:
    int fd_;
};
