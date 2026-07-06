#include "RespParser.h"

#include <cctype>
#include <charconv>

namespace {

// Strict integer parse: whole string must be a (possibly negative) decimal.
bool parse_long(std::string_view s, long& out) {
    if (s.empty()) return false;
    auto [end, ec] = std::from_chars(s.data(), s.data() + s.size(), out);
    return ec == std::errc() && end == s.data() + s.size();
}

}  // namespace

bool RespParser::take_line(Buffer& in, std::string& line) {
    auto v = in.readable();
    auto pos = v.find("\r\n");
    if (pos == std::string_view::npos) return false;
    line.assign(v.substr(0, pos));
    in.consume(pos + 2);
    return true;
}

std::vector<std::string> RespParser::split_inline(const std::string& line) {
    std::vector<std::string> out;
    std::size_t i = 0;
    while (i < line.size()) {
        while (i < line.size() && std::isspace(static_cast<unsigned char>(line[i])))
            ++i;
        if (i >= line.size()) break;
        std::size_t start = i;
        while (i < line.size() && !std::isspace(static_cast<unsigned char>(line[i])))
            ++i;
        out.push_back(line.substr(start, i - start));
    }
    return out;
}

ParseStatus RespParser::parse(Buffer& in, std::vector<std::string>& args) {
    for (;;) {
        switch (state_) {

        case State::ArrayHeader: {
            std::string line;
            if (!take_line(in, line)) {
                // A header line that never terminates is a malformed (or
                // hostile) client -- don't buffer it without bound.
                if (in.size() > kMaxLine) return ParseStatus::Error;
                return ParseStatus::NeedMore;
            }
            if (line.empty()) break;  // blank line: ignore, same as real Redis
            if (line[0] != '*') {
                // INLINE command: plain whitespace-split text, not a
                // multibulk array. Complete immediately -- no BulkHeader/
                // BulkPayload states involved.
                auto tokens = split_inline(line);
                if (tokens.empty()) break;  // all-whitespace line: ignore
                args = std::move(tokens);
                return ParseStatus::Complete;
            }
            long n = 0;
            if (!parse_long(std::string_view(line).substr(1), n))
                return ParseStatus::Error;
            if (n < 0 || n > kMaxArgs) return ParseStatus::Error;
            if (n == 0) break;  // "*0\r\n": empty command, skip silently
            remaining_ = n;
            args_.clear();
            args_.reserve(static_cast<std::size_t>(n));
            state_ = State::BulkHeader;
            break;
        }

        case State::BulkHeader: {
            std::string line;
            if (!take_line(in, line)) {
                if (in.size() > kMaxLine) return ParseStatus::Error;
                return ParseStatus::NeedMore;
            }
            if (line.empty() || line[0] != '$') return ParseStatus::Error;
            long len = 0;
            if (!parse_long(std::string_view(line).substr(1), len))
                return ParseStatus::Error;
            // $-1 (null bulk) is a *reply* construct; inside a command
            // array it's a protocol error, as is anything oversized.
            if (len < 0 || len > kMaxBulk) return ParseStatus::Error;
            bulk_len_ = len;
            state_ = State::BulkPayload;
            break;
        }

        case State::BulkPayload: {
            auto v = in.readable();
            const auto need = static_cast<std::size_t>(bulk_len_) + 2;
            if (v.size() < need) return ParseStatus::NeedMore;
            // Counted read: payload may legally contain \r\n. Only the two
            // bytes AFTER the payload must be the terminator.
            if (v[static_cast<std::size_t>(bulk_len_)] != '\r' ||
                v[static_cast<std::size_t>(bulk_len_) + 1] != '\n')
                return ParseStatus::Error;
            // Copy BEFORE consume -- consume invalidates the view.
            args_.emplace_back(v.substr(0, static_cast<std::size_t>(bulk_len_)));
            in.consume(need);

            if (--remaining_ == 0) {
                args = std::move(args_);
                args_.clear();
                state_ = State::ArrayHeader;
                return ParseStatus::Complete;
            }
            state_ = State::BulkHeader;
            break;
        }
        }
    }
}