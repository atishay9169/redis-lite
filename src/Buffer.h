#pragma once

#include <cstddef>
#include <cstring>
#include <string_view>
#include <vector>

// Growable byte buffer with a read offset.
//
// Consumers read via readable() + consume(n); producers append at the end.
// Consuming only advances read_pos_ -- no memmove per command. Leftover
// bytes are compacted to the front only when the dead prefix dominates,
// keeping the amortized cost low. This is the "track a parse offset,
// compact occasionally" design.
class Buffer {
public:
    void append(const char* data, std::size_t len) {
        data_.insert(data_.end(), data, data + len);
    }

    // Contiguous view of unconsumed bytes. Invalidated by append/consume.
    std::string_view readable() const {
        return {data_.data() + read_pos_, data_.size() - read_pos_};
    }

    void consume(std::size_t n) {
        read_pos_ += n;
        if (read_pos_ == data_.size()) {
            data_.clear();
            read_pos_ = 0;
        } else if (read_pos_ > 4096 && read_pos_ > data_.size() / 2) {
            compact();
        }
    }

    bool empty() const { return read_pos_ == data_.size(); }
    std::size_t size() const { return data_.size() - read_pos_; }

private:
    void compact() {
        std::memmove(data_.data(), data_.data() + read_pos_,
                     data_.size() - read_pos_);
        data_.resize(data_.size() - read_pos_);
        read_pos_ = 0;
    }

    std::vector<char> data_;
    std::size_t read_pos_ = 0;
};
