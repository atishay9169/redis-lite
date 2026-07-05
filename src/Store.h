#pragma once

#include <optional>
#include <string>
#include <unordered_map>

// The keyspace. Single-threaded loop => no locks anywhere by design.
//
// TODO(phase 5): TTL.
//   - expiry map or per-entry deadline (std::chrono::steady_clock).
//   - lazy expiry: get() checks the deadline and deletes on the spot.
//   - active expiry: min-heap ordered by deadline; the loop asks
//     next_expiry_ms() and passes it as the epoll_wait timeout, then calls
//     expire_due() when it wakes. Watch for stale heap entries after
//     SET overwrites a key's TTL (common fix: version/generation counter).
//
// TODO(later, optional): swap unordered_map for your own open-addressing
// table -- ties into the cache-locality story from the order book project.
class Store {
public:
    void set(std::string key, std::string value) {
        map_[std::move(key)] = std::move(value);
    }

    std::optional<std::string> get(const std::string& key) const {
        auto it = map_.find(key);
        if (it == map_.end()) return std::nullopt;
        return it->second;
    }

    bool del(const std::string& key) { return map_.erase(key) > 0; }

private:
    std::unordered_map<std::string, std::string> map_;
};
