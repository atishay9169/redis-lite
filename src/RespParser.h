#pragma once

#include <string>
#include <vector>

#include "Buffer.h"

enum class ParseStatus { Complete, NeedMore, Error };

// Incremental, resumable RESP parser -- one instance per Connection.
//
// State machine: ArrayHeader -> BulkHeader -> BulkPayload -> (BulkHeader ...
// until all args read) -> Complete, reset to ArrayHeader.
//
// Bytes are consumed as each element completes; a partial element consumes
// nothing and parsing resumes at the same state on the next read. Payloads
// are taken by counted length ($len), never scanned -- binary-safe.
class RespParser {
public:
    // Complete: `args` filled, parser reset for the next command.
    // NeedMore: call again once more bytes arrive. Consumed nothing partial.
    // Error:    protocol violation; caller replies -ERR and closes.
    ParseStatus parse(Buffer& in, std::vector<std::string>& args);

private:
    enum class State { ArrayHeader, BulkHeader, BulkPayload };

    // Extract one \r\n-terminated line (without the terminator).
    // Returns false (consuming nothing) if the terminator hasn't arrived.
    static bool take_line(Buffer& in, std::string& line);

    State state_ = State::ArrayHeader;
    long remaining_ = 0;   // bulk strings still expected for this command
    long bulk_len_ = -1;   // declared payload length of the current bulk
    std::vector<std::string> args_;

    static constexpr long kMaxArgs = 1024;                 // *N cap
    static constexpr long kMaxBulk = 64L * 1024 * 1024;    // $len cap, 64 MB
    static constexpr std::size_t kMaxLine = 64 * 1024;     // header-line cap
};
