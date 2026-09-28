// SPDX-License-Identifier: GPL-3.0-or-later
// SPDX-FileCopyrightText: 2026 Genesis-0 contributors

#include "extensions/Origins.h"

#include "core/JsonStore.h"

#include <nlohmann/json.hpp>

#include <system_error>

namespace genesis::extensions {

namespace {

// The store-relative filename the origin map persists under. It is a flat
// `{"<id>": "<origin>"}` object; an id absent from it is Developer, the
// historical default for a store entry.
inline constexpr const char *kOriginsFile = "origins.json";

} // namespace

std::optional<Origin> try_parse_origin(const std::string &text)
{
    if (text == "builtin") {
        return Origin::Builtin;
    }
    if (text == "curated") {
        return Origin::Curated;
    }
    if (text == "developer") {
        return Origin::Developer;
    }
    if (text == "user") {
        return Origin::User;
    }
    return std::nullopt;
}

Origin parse_origin(const std::string &text)
{
    return try_parse_origin(text).value_or(Origin::Developer);
}

const char *origin_name(Origin origin)
{
    switch (origin) {
    case Origin::Builtin:
        return "builtin";
    case Origin::Curated:
        return "curated";
    case Origin::Developer:
        return "developer";
    case Origin::User:
        return "user";
    }
    return "developer";
}

std::map<std::string, Origin> read_origins(const std::filesystem::path &store_root)
{
    std::map<std::string, Origin> origins;
    const std::optional<nlohmann::json> parsed = core::read_json_file(store_root / kOriginsFile);
    if (!parsed) {
        return origins; // absent or unreadable: every id falls back
    }
    const nlohmann::json &doc = *parsed;
    if (!doc.is_object()) {
        return origins;
    }
    for (const auto &[id, value] : doc.items()) {
        if (value.is_string()) {
            origins[id] = parse_origin(value.get<std::string>());
        }
    }
    return origins;
}

bool record_origin(const std::filesystem::path &store_root, const std::string &id, Origin origin)
{
    // Read-modify-write: the latest record for an id wins, and every other id's
    // record survives.
    std::map<std::string, Origin> origins = read_origins(store_root);
    origins[id] = origin;

    nlohmann::json doc = nlohmann::json::object();
    for (const auto &[entry_id, entry_origin] : origins) {
        doc[entry_id] = origin_name(entry_origin);
    }

    return core::write_json_atomic(store_root / kOriginsFile, doc);
}

} // namespace genesis::extensions
