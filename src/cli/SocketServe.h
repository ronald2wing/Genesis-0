// SPDX-License-Identifier: GPL-3.0-or-later
// SPDX-FileCopyrightText: 2026 Genesis-0 contributors

#pragma once

#include <string>

namespace genesis::cli {

// The conventional default socket path: `$XDG_RUNTIME_DIR/genesis-cli.<pid>.sock`
// when XDG_RUNTIME_DIR is set, else `/tmp/genesis-cli.<pid>.sock`. The pid in
// the name keeps concurrent servers apart, so a second `serve` in the same
// session binds its own socket rather than colliding.
std::string default_socket_path();

// Serves the host JSON-RPC API over a Unix domain socket at `path`: binds,
// listens, and accepts one client at a time, answering line-delimited JSON-RPC
// (one request per line, one reply per line) with the same codec as the stdio
// `api` verb. The loop ends on a `shutdown` request, SIGINT, or SIGTERM, after
// which the socket is closed and `path` is unlinked. A stale socket file from
// a previous run is removed before binding; a non-socket file at `path` is
// refused. Returns EXIT_OK on a clean shutdown, EXIT_FAIL when the socket
// cannot be bound.
int cmd_serve(const std::string &path);

} // namespace genesis::cli
