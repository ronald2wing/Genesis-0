// SPDX-License-Identifier: GPL-3.0-or-later
// SPDX-FileCopyrightText: 2026 Genesis-0 contributors

#include "extensions/Sources.h"

#include <nlohmann/json.hpp>

#include <fstream>
#include <system_error>

namespace genesis::extensions {

namespace {

// The store-relative filename the source map persists under. It is a flat
// `{"<id>": {"type": "...", "url": ...}}` object; an id absent from it has no
// recorded source, so a config export reports its source as absent.
inline constexpr const char *kSourcesFile = "sources.json";

// A staging name used only while the map is being rewritten, so a reader never
// sees a half-written map.
inline constexpr const char *kStagingFile = ".tmp-sources.json";

} // namespace

const char *source_type_name(SourceType type)
{
    switch (type) {
    case SourceType::Dir:
        return "dir";
    case SourceType::Git:
        return "git";
    case SourceType::Builtin:
        return "builtin";
    }
    return "dir";
}

std::optional<SourceType> parse_source_type(const std::string &text)
{
    if (text == "dir") {
        return SourceType::Dir;
    }
    if (text == "git") {
        return SourceType::Git;
    }
    if (text == "builtin") {
        return SourceType::Builtin;
    }
    return std::nullopt;
}

std::map<std::string, InstallSource> read_sources(const std::filesystem::path &store_root)
{
    std::map<std::string, InstallSource> sources;
    std::ifstream in(store_root / kSourcesFile);
    if (!in) {
        return sources; // absent or unreadable: every id has no source
    }
    nlohmann::json doc;
    try {
        in >> doc;
    } catch (const nlohmann::json::exception &) {
        return sources; // a corrupt map is treated as empty
    }
    if (!doc.is_object()) {
        return sources;
    }
    for (const auto &[id, value] : doc.items()) {
        if (!value.is_object()) {
            continue;
        }
        InstallSource source;
        const auto type_it = value.find("type");
        if (type_it != value.end() && type_it->is_string()) {
            source.type = parse_source_type(type_it->get<std::string>()).value_or(SourceType::Dir);
        }
        const auto url_it = value.find("url");
        if (url_it != value.end() && url_it->is_string()) {
            source.url = url_it->get<std::string>();
        }
        const auto ref_it = value.find("ref");
        if (ref_it != value.end() && ref_it->is_string()) {
            source.ref = ref_it->get<std::string>();
        }
        const auto subdir_it = value.find("subdir");
        if (subdir_it != value.end() && subdir_it->is_string()) {
            source.subdir = subdir_it->get<std::string>();
        }
        sources[id] = std::move(source);
    }
    return sources;
}

bool record_source(const std::filesystem::path &store_root, const std::string &id,
                   InstallSource source)
{
    // Read-modify-write: the latest record for an id wins, and every other id's
    // record survives.
    std::map<std::string, InstallSource> sources = read_sources(store_root);
    sources[id] = std::move(source);

    nlohmann::json doc = nlohmann::json::object();
    for (const auto &[entry_id, entry_source] : sources) {
        nlohmann::json entry = nlohmann::json::object();
        entry["type"] = source_type_name(entry_source.type);
        if (entry_source.type != SourceType::Builtin) {
            entry["url"] = entry_source.url;
        }
        if (entry_source.type == SourceType::Git && !entry_source.ref.empty()) {
            entry["ref"] = entry_source.ref;
        }
        if (entry_source.type == SourceType::Git && !entry_source.subdir.empty()) {
            entry["subdir"] = entry_source.subdir;
        }
        doc[entry_id] = std::move(entry);
    }

    std::error_code ec;
    std::filesystem::create_directories(store_root, ec);
    if (ec) {
        return false;
    }

    // Write beside the destination (same filesystem) and rename, so the map is
    // never observed half-written.
    const std::filesystem::path staging = store_root / kStagingFile;
    const std::filesystem::path final = store_root / kSourcesFile;

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
