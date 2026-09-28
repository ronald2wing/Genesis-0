// SPDX-License-Identifier: GPL-3.0-or-later
// SPDX-FileCopyrightText: 2026 Genesis-0 contributors

#pragma once

#include <cstddef>
#include <cstdint>
#include <optional>
#include <string>

#include <nlohmann/json.hpp>

#include "api/Api.h"

namespace genesis::api {

// The largest JSON-RPC request line a transport accepts: 1 MiB. Shared so the
// stdio `api` verb and the socket `serve` verb both bound a request line before
// the parser sees it, and so the oversize reply is spelled once.
inline constexpr std::size_t kMaxJsonRequestBytes = 1u << 20;

// A transport-level failure, before dispatch is reached: an unparsable line,
// an unknown method, or a method whose params this codec cannot decode.
struct DecodeError
{
    std::int64_t code = -32600;
    std::string message;
};

// The JSON-RPC error and result envelopes, shared across every transport.
nlohmann::json wire_error(const nlohmann::json &id, std::int64_t code, const std::string &message);
nlohmann::json wire_result(nlohmann::json id, const nlohmann::json &result);

// The reply for a request line that exceeds kMaxJsonRequestBytes: a JSON-RPC
// parse error, identical across every transport so a client cannot tell one
// from another.
std::string oversize_request_reply();

// Returns a JSON-RPC parse error when `line` exceeds the size or nesting
// guards, or nullopt when it is within bounds. The scan tracks string literals
// and `\` escapes so a brace inside a string is never miscounted.
std::optional<std::string> json_structure_guard(const std::string &line);

// Maps one wire method name and its params onto the API's typed request. An
// unknown method, or one whose params have no JSON codec here, fills `error`
// and returns nullopt.
std::optional<Request> decode_request(const std::string &method, const nlohmann::json &params,
                                      DecodeError &error);

// Encodes a dispatch reply as the JSON its method documents.
nlohmann::json reply_to_json(const Reply &reply);

} // namespace genesis::api
