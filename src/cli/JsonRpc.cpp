// SPDX-License-Identifier: GPL-3.0-or-later
// SPDX-FileCopyrightText: 2026 Genesis-0 contributors

#include "cli/JsonRpc.h"

#include <nlohmann/json.hpp>

#include <algorithm>
#include <cctype>
#include <optional>
#include <string>
#include <utility>

#include "api/Api.h"
#include "project/Editor.h"

namespace genesis::cli {

namespace {

using nlohmann::json;

namespace api = genesis::api;

// Whether a line is blank or all whitespace: a keep-alive the stream may skip.
bool is_blank(const std::string &line)
{
    return std::all_of(line.begin(), line.end(),
                       [](unsigned char c) { return std::isspace(c) != 0; });
}

std::string string_at(const json &obj, const char *key)
{
    if (!obj.is_object()) {
        return { };
    }
    const auto it = obj.find(key);
    if (it == obj.end() || !it->is_string()) {
        return { };
    }
    return it->get<std::string>();
}

// The object under `params`, or an empty object; a request with no params is
// legal for the methods that take none.
const json &params_of(const json &line)
{
    static const json empty = json::object();
    if (!line.is_object()) {
        return empty;
    }
    const auto it = line.find("params");
    if (it == line.end() || !it->is_object()) {
        return empty;
    }
    return *it;
}

} // namespace

// The reply line for one input line, exactly as `api` would write it over
// stdio. Returns nullopt for a blank keep-alive line (nothing to write).
// `shutdown` is flipped when the line is the server's `shutdown` method, so a
// transport can end its loop; every other method routes through
// `api::dispatch`. Never throws: a malformed line becomes a JSON-RPC error
// reply rather than a crash.
std::optional<std::string> handle_jsonrpc_line(project::Editor &editor,
                                               const api::Services &services,
                                               const std::string &line, bool &shutdown)
{
    if (is_blank(line)) {
        return std::nullopt;
    }
    if (const std::optional<std::string> guarded = api::json_structure_guard(line)) {
        return guarded;
    }
    json request;
    try {
        request = json::parse(line);
    } catch (const json::parse_error &e) {
        return api::wire_error(nullptr, -32700, e.what()).dump();
    }
    if (!request.is_object()) {
        return api::wire_error(nullptr, -32600, "request must be a JSON object").dump();
    }
    const json id = request.contains("id") ? request["id"] : json(nullptr);
    const std::string method = string_at(request, "method");
    if (method.empty()) {
        return api::wire_error(id, -32600, "request has no method").dump();
    }
    if (method == "shutdown") {
        shutdown = true;
        return api::wire_result(id, json::object()).dump();
    }
    api::DecodeError error;
    const std::optional<api::Request> decoded =
            api::decode_request(method, params_of(request), error);
    if (!decoded) {
        return api::wire_error(id, error.code, error.message).dump();
    }
    const api::Response response = api::dispatch(*decoded, editor, services);
    if (response) {
        return api::wire_result(id, api::reply_to_json(*response)).dump();
    }
    return api::wire_error(id, api::number(response.error().code), response.error().message).dump();
}

} // namespace genesis::cli
