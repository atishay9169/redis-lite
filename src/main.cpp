#include <cstdint>
#include <cstdio>
#include <cstdlib>

#include "EventLoop.h"

int main(int argc, char** argv) {
    std::uint16_t port = 6380;  // 6380 so a real redis-server on 6379 can
                                // run alongside as a behavior reference
    if (argc > 1) port = static_cast<std::uint16_t>(std::atoi(argv[1]));

    EventLoop loop(port);
    loop.run();  // never returns
    return 0;
}
