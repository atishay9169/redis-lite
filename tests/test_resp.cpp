// Parser unit tests. Build:
//   g++ -std=c++20 -fsanitize=address,undefined -I src ...
//       tests/test_resp.cpp src/RespParser.cpp -o test_resp && ./test_resp
#include <cassert>
#include <cstdio>
#include <string>
#include <vector>

#include "../src/RespParser.h"

using Args = std::vector<std::string>;

int main() {
    // 1. Simple complete command.
    {
        RespParser p; Buffer b; Args a;
        std::string s = "*3\r\n$3\r\nSET\r\n$3\r\nfoo\r\n$3\r\nbar\r\n";
        b.append(s.data(), s.size());
        assert(p.parse(b, a) == ParseStatus::Complete);
        assert((a == Args{"SET", "foo", "bar"}));
        assert(b.empty());
    }

    // 2. Byte-by-byte delivery: NeedMore at every prefix, Complete at end.
    {
        RespParser p; Buffer b; Args a;
        std::string s = "*2\r\n$4\r\nECHO\r\n$2\r\nhi\r\n";
        for (std::size_t i = 0; i + 1 < s.size(); ++i) {
            b.append(&s[i], 1);
            assert(p.parse(b, a) == ParseStatus::NeedMore);
        }
        b.append(&s[s.size() - 1], 1);
        assert(p.parse(b, a) == ParseStatus::Complete);
        assert((a == Args{"ECHO", "hi"}));
    }

    // 3. Pipelining: two commands in one buffer, parsed in two calls.
    {
        RespParser p; Buffer b; Args a;
        std::string s = "*2\r\n$3\r\nGET\r\n$1\r\nk\r\n"
                        "*1\r\n$4\r\nPING\r\n";
        b.append(s.data(), s.size());
        assert(p.parse(b, a) == ParseStatus::Complete);
        assert((a == Args{"GET", "k"}));
        assert(p.parse(b, a) == ParseStatus::Complete);
        assert((a == Args{"PING"}));
        assert(p.parse(b, a) == ParseStatus::NeedMore);
    }

    // 4. Binary-safe payload: value contains \r\n.
    {
        RespParser p; Buffer b; Args a;
        std::string val = "ab\r\ncd";
        std::string s = "*3\r\n$3\r\nSET\r\n$1\r\nk\r\n$6\r\n" + val + "\r\n";
        b.append(s.data(), s.size());
        assert(p.parse(b, a) == ParseStatus::Complete);
        assert(a[2] == val);
    }

    // 5. Malformed input.
    {
        RespParser p; Buffer b; Args a;               // wrong array prefix
        std::string s = "?3\r\n";
        b.append(s.data(), s.size());
        assert(p.parse(b, a) == ParseStatus::Error);
    }
    {
        RespParser p; Buffer b; Args a;               // wrong bulk prefix
        std::string s = "*1\r\n#4\r\nPING\r\n";
        b.append(s.data(), s.size());
        assert(p.parse(b, a) == ParseStatus::Error);
    }
    {
        RespParser p; Buffer b; Args a;               // negative bulk length
        std::string s = "*1\r\n$-1\r\n";
        b.append(s.data(), s.size());
        assert(p.parse(b, a) == ParseStatus::Error);
    }
    {
        RespParser p; Buffer b; Args a;               // non-numeric count
        std::string s = "*abc\r\n";
        b.append(s.data(), s.size());
        assert(p.parse(b, a) == ParseStatus::Error);
    }
    {
        RespParser p; Buffer b; Args a;  // payload longer than declared:
        std::string s = "*1\r\n$2\r\nabc\r\n";  // byte after len isn't \r
        b.append(s.data(), s.size());
        assert(p.parse(b, a) == ParseStatus::Error);
    }

    // 6. Unterminated header line past the cap -> Error, not endless buffering.
    {
        RespParser p; Buffer b; Args a;
        std::string s(70 * 1024, 'x');  // no \r\n anywhere
        b.append(s.data(), s.size());
        assert(p.parse(b, a) == ParseStatus::Error);
    }

    // 7. Empty command "*0" is skipped, then a real command parses.
    {
        RespParser p; Buffer b; Args a;
        std::string s = "*0\r\n*1\r\n$4\r\nPING\r\n";
        b.append(s.data(), s.size());
        assert(p.parse(b, a) == ParseStatus::Complete);
        assert((a == Args{"PING"}));
    }

    std::puts("all parser tests passed");
    return 0;
}
