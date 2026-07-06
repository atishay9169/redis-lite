#pragma once

#include <chrono>
#include <optional>
#include <queue>
#include <string>
#include <unordered_map>
#include <vector>

// The keyspace, now with TTL. Single-threaded loop => no locks anywhere.
//
// Two expiry mechanisms, each covering the other's gap:
//   - Lazy:   get() checks the deadline and deletes on the spot. Cheap,
//             but a key that's never touched again would leak forever.
//   - Active: a min-heap of (deadline, key) lets the loop ask "how long
//             until the next key dies" and use that as its epoll_wait
//             timeout -- so idle expired keys get reclaimed even with no
//             client asking.
//
// Stale heap entries: overwriting a key's TTL (via set()) does NOT remove
// its old heap entry (binary heaps can't cheaply delete from the middle).
// So when a heap entry pops, its deadline is compared against the key's
// CURRENT deadline in the map -- if they don't match, the entry is stale
// garbage from a since-overwritten key and is discarded, deleting nothing.
// This is "re-check-on-pop."
class Store {
public:
    using Clock = std::chrono::steady_clock;
    using TimePoint = Clock::time_point;

    // SET key value -- no TTL, clears any existing one (matches Redis:
    // a plain SET always drops the old expiry).
    void set(std::string key, std::string value) {
        map_[std::move(key)] = Entry{std::move(value), std::nullopt};
    }

    // SET key value EX/PX ... -- ttl is relative from now.
    void set(std::string key, std::string value, std::chrono::milliseconds ttl) {
        TimePoint deadline = Clock::now() + ttl;
        map_[key] = Entry{std::move(value), deadline};
        heap_.push({deadline, std::move(key)});
    }

    std::optional<std::string> get(const std::string& key) {
        auto it = map_.find(key);
        if (it == map_.end()) return std::nullopt;
        if (is_expired(it->second)) {
            map_.erase(it);
            return std::nullopt;
        }
        return it->second.value;
    }

    bool del(const std::string& key) { return map_.erase(key) > 0; }

    // EXPIRE key seconds -- returns false if the key doesn't exist.
    bool expire(const std::string& key, std::chrono::seconds ttl) {
        auto it = map_.find(key);
        if (it == map_.end() || is_expired(it->second)) return false;
        TimePoint deadline = Clock::now() + ttl;
        it->second.deadline = deadline;
        heap_.push({deadline, key});
        return true;
    }

    // TTL key -- Redis semantics: -2 absent, -1 no expiry, else seconds
    // remaining (ceiling, so a key with 400ms left reports 1, never 0
    // while still alive).
    long long ttl_seconds(const std::string& key) {
        auto it = map_.find(key);
        if (it == map_.end()) return -2;
        if (is_expired(it->second)) {
            map_.erase(it);
            return -2;
        }
        if (!it->second.deadline) return -1;
        auto remaining = *it->second.deadline - Clock::now();
        auto us = std::chrono::duration_cast<std::chrono::microseconds>(remaining).count();
        return (us + 999'999) / 1'000'000;  // ceiling to whole seconds
    }

    // Milliseconds until the next key is due, for the epoll_wait timeout.
    // -1 means "nothing pending, block indefinitely." Discards stale/dead
    // heap garbage as it peeks, but never deletes a live key -- that's
    // expire_due()'s job.
    long next_expiry_ms() {
        drop_stale_heap_top();
        if (heap_.empty()) return -1;

        auto remaining = heap_.top().deadline - Clock::now();
        auto us = std::chrono::duration_cast<std::chrono::microseconds>(remaining).count();
        if (us <= 0) return 0;  // already due; let epoll_wait return immediately
        return static_cast<long>((us + 999) / 1000);  // round UP, ae_epoll.c-style
    }

    // Pop and delete every key whose deadline has passed. Call this on
    // every loop wakeup (timeout or event), not just on timeout -- a
    // client request can arrive at the same moment a key expires.
    void expire_due() {
        auto now = Clock::now();
        for (;;) {
            drop_stale_heap_top();
            if (heap_.empty() || heap_.top().deadline > now) break;
            map_.erase(heap_.top().key);  // heap_.top() already validated
                                          // live+current by drop_stale_heap_top
            heap_.pop();
        }
    }

private:
    struct Entry {
        std::string value;
        std::optional<TimePoint> deadline;
    };

    struct HeapItem {
        TimePoint deadline;
        std::string key;
    };
    struct HeapItemGreater {
        bool operator()(const HeapItem& a, const HeapItem& b) const {
            return a.deadline > b.deadline;  // min-heap: soonest deadline on top
        }
    };

    bool is_expired(const Entry& e) const {
        return e.deadline && *e.deadline <= Clock::now();
    }

    // Discard heap entries at the top that are stale (key gone, or key's
    // current deadline no longer matches this heap entry -- meaning the
    // key was SET/EXPIRE'd again since this entry was pushed). Leaves the
    // heap either empty or with a genuinely live, current top.
    void drop_stale_heap_top() {
        while (!heap_.empty()) {
            const auto& top = heap_.top();
            auto it = map_.find(top.key);
            if (it != map_.end() && it->second.deadline == top.deadline) {
                break;  // top is valid and current
            }
            heap_.pop();  // stale: key gone, or superseded by a later SET/EXPIRE
        }
    }

    std::unordered_map<std::string, Entry> map_;
    std::priority_queue<HeapItem, std::vector<HeapItem>, HeapItemGreater> heap_;
};