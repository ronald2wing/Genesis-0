// SPDX-License-Identifier: GPL-3.0-or-later
// SPDX-FileCopyrightText: 2026 Genesis-0 contributors
//
// Typed reads from a JSON-RPC params object. Both ends of the wire need them —
// `api::decode_request` turns params into a Request, and the CLI's transport
// reads `method`/`id` from the envelope — so they live here rather than in a
// private block in each translation unit. A wrong or missing type yields the
// fallback, never an exception: the caller reports a decode error instead.

#pragma once

#include <optional>
#include <string>

#include <nlohmann/json.hpp>

namespace genesis::api {

inline std::string string_at(const nlohmann::json &object, const char *key)
{
    if (!object.is_object()) {
        return { };
    }
    const auto entry = object.find(key);
    if (entry == object.end() || !entry->is_string()) {
        return { };
    }
    return entry->get<std::string>();
}

inline std::optional<std::string> opt_string_at(const nlohmann::json &object, const char *key)
{
    if (!object.is_object()) {
        return std::nullopt;
    }
    const auto entry = object.find(key);
    if (entry == object.end() || !entry->is_string()) {
        return std::nullopt;
    }
    return entry->get<std::string>();
}

inline bool bool_at(const nlohmann::json &object, const char *key, bool fallback = false)
{
    if (!object.is_object()) {
        return fallback;
    }
    const auto entry = object.find(key);
    if (entry == object.end() || !entry->is_boolean()) {
        return fallback;
    }
    return entry->get<bool>();
}

inline std::optional<bool> opt_bool_at(const nlohmann::json &object, const char *key)
{
    if (!object.is_object()) {
        return std::nullopt;
    }
    const auto entry = object.find(key);
    if (entry == object.end() || !entry->is_boolean()) {
        return std::nullopt;
    }
    return entry->get<bool>();
}

} // namespace genesis::api
