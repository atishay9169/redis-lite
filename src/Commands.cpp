#include "Commands.h"

#include <cctype>
#include <charconv>
#include <chrono>

namespace {

std::string upper(std::string_view s) {
    std::string u(s);
    for (char& c : u)
        c = static_cast<char>(std::toupper(static_cast<unsigned char>(c)));
    return u;
}

void reply_arity_error(Buffer& out, std::string_view cmd) {
    std::string msg = "wrong number of arguments for '";
    msg += cmd;
    msg += "' command";
    reply_error(out, msg);
}

// Strict positive-integer parse (TTLs: no sign, no trailing junk).
bool parse_positive_long(std::string_view s, long long& out) {
    if (s.empty()) return false;
    auto [end, ec] = std::from_chars(s.data(), s.data() + s.size(), out);
    return ec == std::errc() && end == s.data() + s.size() && out > 0;
}

}  // namespace

void reply_simple(Buffer& out, std::string_view s) {
    out.append("+", 1);
    out.append(s.data(), s.size());
    out.append("\r\n", 2);
}

void reply_error(Buffer& out, std::string_view msg) {
    out.append("-ERR ", 5);
    out.append(msg.data(), msg.size());
    out.append("\r\n", 2);
}

void reply_bulk(Buffer& out, std::string_view s) {
    std::string hdr = "$" + std::to_string(s.size()) + "\r\n";
    out.append(hdr.data(), hdr.size());
    out.append(s.data(), s.size());
    out.append("\r\n", 2);
}

void reply_nil(Buffer& out) { out.append("$-1\r\n", 5); }

void reply_int(Buffer& out, long long v) {
    std::string s = ":" + std::to_string(v) + "\r\n";
    out.append(s.data(), s.size());
}

void execute_command(const std::vector<std::string>& args, Store& store,
                     Buffer& out) {
    // Parser guarantees at least one element (*0 is skipped upstream).
    const std::string cmd = upper(args[0]);

    if (cmd == "PING") {
        if (args.size() == 1) reply_simple(out, "PONG");
        else if (args.size() == 2) reply_bulk(out, args[1]);
        else reply_arity_error(out, "ping");

    } else if (cmd == "ECHO") {
        if (args.size() != 2) reply_arity_error(out, "echo");
        else reply_bulk(out, args[1]);

    } else if (cmd == "SET") {
        if (args.size() == 3) {
            store.set(args[1], args[2]);
            reply_simple(out, "OK");
        } else if (args.size() == 5) {
            std::string opt = upper(args[3]);
            long long n = 0;
            if (!parse_positive_long(args[4], n)) {
                reply_error(out, "value is not an integer or out of range");
            } else if (opt == "EX") {
                store.set(args[1], args[2], std::chrono::seconds(n));
                reply_simple(out, "OK");
            } else if (opt == "PX") {
                store.set(args[1], args[2], std::chrono::milliseconds(n));
                reply_simple(out, "OK");
            } else {
                reply_error(out, "syntax error");
            }
        } else {
            reply_arity_error(out, "set");
        }

    } else if (cmd == "GET") {
        if (args.size() != 2) reply_arity_error(out, "get");
        else if (auto v = store.get(args[1])) reply_bulk(out, *v);
        else reply_nil(out);

    } else if (cmd == "DEL") {
        if (args.size() < 2) reply_arity_error(out, "del");
        else {
            long long n = 0;
            for (std::size_t i = 1; i < args.size(); ++i)
                if (store.del(args[i])) ++n;
            reply_int(out, n);
        }

    } else if (cmd == "EXPIRE") {
        if (args.size() != 3) {
            reply_arity_error(out, "expire");
        } else {
            long long n = 0;
            if (!parse_positive_long(args[2], n)) {
                reply_error(out, "value is not an integer or out of range");
            } else {
                bool did = store.expire(args[1], std::chrono::seconds(n));
                reply_int(out, did ? 1 : 0);
            }
        }

    } else if (cmd == "TTL") {
        if (args.size() != 2) reply_arity_error(out, "ttl");
        else reply_int(out, store.ttl_seconds(args[1]));

    } else if (cmd == "COMMAND") {
        // redis-cli probes this on startup; an empty array keeps it happy.
        out.append("*0\r\n", 4);

    } else {
        std::string msg = "unknown command '";
        msg += args[0];
        msg += "'";
        reply_error(out, msg);
    }
}
