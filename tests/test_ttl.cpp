// Store/TTL unit tests. Build:
//   g++ -std=c++20 -fsanitize=address,undefined -I src ...
//       tests/test_ttl.cpp -o test_ttl && ./test_ttl
#include <cassert>
#include <chrono>
#include <cstdio>
#include <thread>

#include "../src/Store.h"

using namespace std::chrono_literals;

int main() {
    // 1. No TTL: plain set/get, never expires.
    {
        Store s;
        s.set("k", "v");
        assert(s.get("k") == "v");
        assert(s.ttl_seconds("k") == -1);  // exists, no expiry
    }

    // 2. TTL absent key.
    {
        Store s;
        assert(s.ttl_seconds("nope") == -2);
        assert(!s.get("nope").has_value());
    }

    // 3. Lazy expiry: get() after the deadline returns nullopt.
    {
        Store s;
        s.set("k", "v", 20ms);
        assert(s.get("k") == "v");
        std::this_thread::sleep_for(40ms);
        assert(!s.get("k").has_value());  // lazy check kills it right here
    }

    // 4. Active expiry: expire_due() reaps a key nobody ever GETs again.
    {
        Store s;
        s.set("k", "v", 20ms);
        std::this_thread::sleep_for(40ms);
        s.expire_due();
        // Can't observe internal map size directly, but ttl_seconds/get
        // must both agree the key is gone after expire_due ran.
        assert(s.ttl_seconds("k") == -2);
    }

    // 5. next_expiry_ms: empty store -> -1 (block forever).
    {
        Store s;
        assert(s.next_expiry_ms() == -1);
    }

    // 6. next_expiry_ms: reflects the soonest pending deadline, roughly.
    {
        Store s;
        s.set("k", "v", 200ms);
        long ms = s.next_expiry_ms();
        assert(ms > 0 && ms <= 200);
    }

    // 7. Stale heap entry: overwrite a key's TTL before it fires; the OLD
    //    heap entry must not delete the NEW value/deadline when it pops.
    {
        Store s;
        s.set("k", "v1", 20ms);       // pushes heap entry A (deadline ~t+20ms)
        s.set("k", "v2", 500ms);      // pushes heap entry B; A is now stale
        std::this_thread::sleep_for(40ms);  // A's deadline has passed; B's hasn't
        s.expire_due();               // must discard stale A, NOT delete "k"
        assert(s.get("k") == "v2");   // k must still be alive with v2
        assert(s.ttl_seconds("k") > 0);
    }

    // 8. Stale heap entry via plain set() (clears TTL entirely): old timed
    //    entry must not later delete a now-permanent key.
    {
        Store s;
        s.set("k", "v1", 20ms);   // pushes heap entry with a deadline
        s.set("k", "v2");         // plain set: clears deadline entirely
        std::this_thread::sleep_for(40ms);
        s.expire_due();
        assert(s.get("k") == "v2");  // must NOT have been deleted
        assert(s.ttl_seconds("k") == -1);
    }

    // 9. EXPIRE on an existing key without a prior TTL.
    {
        Store s;
        s.set("k", "v");
        assert(s.expire("k", 1s) == true);
        long long t = s.ttl_seconds("k");
        assert(t == 1 || t == 0 || t == 1);  // ~1s, ceiling-rounded
        assert(t >= 0);
    }

    // 10. EXPIRE on a missing key returns false.
    {
        Store s;
        assert(s.expire("nope", 1s) == false);
    }

    // 11. DEL removes a key with a pending TTL; its stale heap entry must
    //     not resurrect/misbehave later.
    {
        Store s;
        s.set("k", "v", 20ms);
        assert(s.del("k") == true);
        std::this_thread::sleep_for(40ms);
        s.expire_due();  // must not crash / must not affect anything else
        assert(s.ttl_seconds("k") == -2);
    }

    // 12. Multiple keys expiring at different times: expire_due() only
    //     reaps the ones actually due, leaves the rest.
    {
        Store s;
        s.set("a", "1", 20ms);
        s.set("b", "2", 500ms);
        std::this_thread::sleep_for(40ms);
        s.expire_due();
        assert(s.ttl_seconds("a") == -2);   // a is gone
        assert(s.get("b") == "2");           // b still alive
    }

    std::puts("all ttl tests passed");
    return 0;
}
