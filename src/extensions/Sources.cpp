// SPDX-License-Identifier: GPL-3.0-or-later
// SPDX-FileCopyrightText: 2026 Genesis-0 contributors

#include "extensions/Sources.h"

#include "core/JsonStore.h"

#include <nlohmann/json.hpp>

#include <system_error>

namespace genesis::extensions {

namespace {

// The store-relative filename the source map persists under. It is a flat
// `{"<id>": {"type": "...", "url": ...}}` object; an id absent from it has no
// recorded source, so a config export reports its source as absent.
inline constexpr const char *kSourcesFile = "sources.json";

} // namespace

const char *source_type_name(SourceType type)
{
    switch (type) {
    case SourceType::Dir:
        return "dir";
    case SourceType::Git:
        return "git";
    case SourceType::Archive:
        return "archive";
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
    if (text == "archive") {
        return SourceType::Archive;
    }
    if (text == "builtin") {
        return SourceType::Builtin;
    }
    return std::nullopt;
}

std::map<std::string, InstallSource> read_sources(const std::filesystem::path &store_root)
{
    std::map<std::string, InstallSource> sources;
    const std::optional<nlohmann::json> parsed = core::read_json_file(store_root / kSourcesFile);
    if (!parsed) {
        return sources; // absent or unreadable: every id has no source
    }
    const nlohmann::json &doc = *parsed;
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
        if ((entry_source.type == SourceType::Git || entry_source.type == SourceType::Archive)
            && !entry_source.subdir.empty()) {
            entry["subdir"] = entry_source.subdir;
        }
        doc[entry_id] = std::move(entry);
    }

    return core::write_json_atomic(store_root / kSourcesFile, doc);
}

} // namespace genesis::extensions
