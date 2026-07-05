#include <cassert>
#include <chrono>
#include <cstdio>
#include <thread>

#include "../src/Store.h"

int main() {
    Store store;
    store.set("k", "v", std::chrono::milliseconds{20});

    assert(store.get("k").has_value());
    assert(store.get("k").value() == "v");

    std::this_thread::sleep_for(std::chrono::milliseconds{40});
    store.expire_due();

    assert(!store.get("k").has_value());
    std::puts("ttl tests passed");
    return 0;
}
