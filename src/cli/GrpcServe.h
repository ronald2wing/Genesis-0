// SPDX-License-Identifier: GPL-3.0-or-later
// SPDX-FileCopyrightText: 2026 Genesis-0 contributors

#pragma once

#include <optional>
#include <string>

namespace genesis::cli {

// The bearer token sources `serve --grpc` accepts, resolved in preference
// order (highest first): the GENESIS_GRPC_TOKEN environment variable, then
// --token-file, then --token. `warning` names the one caveat the operator must
// know when --token is the chosen source: the secret is visible in the process
// list (ps). `error` names a hard failure (an unreadable or empty token file).
struct TokenResolution
{
    std::optional<std::string> token;
    std::string warning;
    std::string error;
};

// Resolves the gRPC bearer token without exposing it in argv where possible.
// When `token_arg` is non-empty and neither the environment variable nor
// `token_file` supplied a token, `token_arg` is used and `warning` is set.
TokenResolution resolve_grpc_token(const std::string &token_arg, const std::string &token_file);

// Serves the host JSON-RPC API over gRPC on `addr` (a "host:port" listen
// address; the CLI defaults it to 127.0.0.1:50051, so the server is
// localhost-only unless the caller names another address). Every Call carries
// one JSON-RPC 2.0 request string and returns one JSON-RPC 2.0 reply string
// through the same codec as the stdio `api` verb and the Unix-socket `serve`
// verb. Calls must present `authorization: Bearer <token>` client metadata; a
// missing or wrong token is refused with UNAUTHENTICATED before dispatch.
// `token` must be non-empty - the server refuses to start without one.
//
// A bind that is not loopback is refused unless `cert_path` and `key_path` both
// name a PEM certificate chain and private key, which enable server-side TLS
// (the bearer token is still checked on top). A loopback bind stays insecure
// when no certificate is given. Binds, logs the bound address, then blocks
// until a `shutdown` request, SIGINT, or SIGTERM. Returns EXIT_OK on a clean
// shutdown, EXIT_FAIL when the token is empty, the bind is refused, the TLS
// material is unusable, or the address cannot be bound.
int cmd_grpc_serve(const std::string &addr, const std::string &token,
                   const std::optional<std::string> &cert_path = std::nullopt,
                   const std::optional<std::string> &key_path = std::nullopt);

} // namespace genesis::cli
