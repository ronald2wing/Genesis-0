// SPDX-License-Identifier: GPL-3.0-or-later
// SPDX-FileCopyrightText: 2026 Genesis-0 contributors

#include "extensions/Origins.h"

#include <nlohmann/json.hpp>

#include <fstream>
#include <system_error>

namespace genesis::extensions {

namespace {

// The store-relative filename the origin map persists under. It is a flat
// `{"<id>": "<origin>"}` object; an id absent from it is Developer, the
// historical default for a store entry.
inline constexpr const char *kOriginsFile = "origins.json";

// A staging name used only while the map is being rewritten, so a reader never
// sees a half-written map.
inline constexpr const char *kStagingFile = ".tmp-origins.json";

} // namespace

Origin parse_origin(const std::string &text)
{
    if (text == "builtin") {
        return Origin::Builtin;
    }
    if (text == "curated") {
        return Origin::Curated;
    }
    if (text == "user") {
        return Origin::User;
    }
    return Origin::Developer; // "developer" or anything unknown
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
    std::ifstream in(store_root / kOriginsFile);
    if (!in) {
        return origins; // absent or unreadable: every id falls back
    }
    nlohmann::json doc;
    try {
        in >> doc;
    } catch (const nlohmann::json::exception &) {
        return origins; // a corrupt map is treated as empty
    }
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

    std::error_code ec;
    std::filesystem::create_directories(store_root, ec);
    if (ec) {
        return false;
    }

    // Write beside the destination (same filesystem) and rename, so the map is
    // never observed half-written.
    const std::filesystem::path staging = store_root / kStagingFile;
    const std::filesystem::path final = store_root / kOriginsFile;

    std::ofstream out(staging, std::ios::trunc);
    if (!out) {
        return false;
    }
    out << doc.dump(2) << '\n';
    out.close();
    if (!out) {
        std::error_code ignored;
        std::filesystem::remove(staging, ignored);
        return false;
    }

    std::filesystem::remove(final, ec);
    ec.clear();
    std::filesystem::rename(staging, final, ec);
    if (ec) {
        std::error_code ignored;
        std::filesystem::remove(staging, ignored);
        return false;
    }
    return true;
}

} // namespace genesis::extensions
