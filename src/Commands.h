#pragma once

#include <string>
#include <string_view>
#include <vector>

#include "Buffer.h"
#include "Store.h"

// Dispatch one parsed command and serialize its RESP reply into `out`.
void execute_command(const std::vector<std::string>& args, Store& store,
                     Buffer& out);

// RESP reply serialization.
void reply_simple(Buffer& out, std::string_view s);   // +OK\r\n
void reply_error(Buffer& out, std::string_view msg);  // -ERR msg\r\n
void reply_bulk(Buffer& out, std::string_view s);     // $3\r\nfoo\r\n
void reply_nil(Buffer& out);                          // $-1\r\n
void reply_int(Buffer& out, long long v);             // :42\r\n
