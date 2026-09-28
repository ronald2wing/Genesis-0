// SPDX-License-Identifier: GPL-3.0-or-later
// SPDX-FileCopyrightText: 2026 Genesis-0 contributors

#pragma once

#include <optional>
#include <string>

#include "api/Codec.h"

namespace genesis::project {
class Editor;
}

namespace genesis::api {
struct Services;
}

namespace genesis::cli {

// Re-exported so the stdio `api` verb and the socket `serve` verb keep reading
// them from here while the codec bodies live in the engine-free api library.
using api::kMaxJsonRequestBytes;
using api::oversize_request_reply;

// The reply line for one JSON-RPC request line, exactly as `api` writes it:
// parses the line, routes it to api::dispatch, and returns the reply to write.
// Returns nullopt for a blank keep-alive line. `shutdown` is set when the line
// is the server's `shutdown` method, so a transport (stdio or socket) can end
// its loop. Never throws: malformed input becomes a JSON-RPC error reply.
std::optional<std::string> handle_jsonrpc_line(project::Editor &editor,
                                               const api::Services &services,
                                               const std::string &line, bool &shutdown);

} // namespace genesis::cli
