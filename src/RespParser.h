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
// Also supports INLINE commands: a line that does NOT start with '*' is
// treated as a plain space-separated command (e.g. "PING\r\n",
// "SET foo bar\r\n"), same as real Redis. This isn't a benchmark-only
// quirk -- it predates the multibulk protocol (originally for telnet
// compatibility) and real clients/tools still use it for quick protocol
// probes (redis-benchmark's startup CONFIG check, for one). Skipping it
// means any such client gets an immediate protocol error and disconnect.
//
// Bytes are consumed as each element completes; a partial element consumes
// nothing and parsing resumes at the same state on the next read. Bulk
// payloads are taken by counted length ($len), never scanned -- binary-safe.
// (Inline args are plain whitespace-split text, so they are NOT
// binary-safe -- this matches real Redis's own inline-command limitation.)
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

    // Split an inline command line on whitespace. Simple split, no quote
    // handling -- matches real Redis's own inline-command limitations.
    static std::vector<std::string> split_inline(const std::string& line);

    State state_ = State::ArrayHeader;
    long remaining_ = 0;   // bulk strings still expected for this command
    long bulk_len_ = -1;   // declared payload length of the current bulk
    std::vector<std::string> args_;

    static constexpr long kMaxArgs = 1024;                 // *N cap
    static constexpr long kMaxBulk = 64L * 1024 * 1024;    // $len cap, 64 MB
    static constexpr std::size_t kMaxLine = 64 * 1024;     // header-line cap
};