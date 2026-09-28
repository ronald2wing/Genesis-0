// SPDX-License-Identifier: GPL-3.0-or-later
// SPDX-FileCopyrightText: 2026 Genesis-0 contributors

#include "extensions/Catalog.h"

#include <cstdint>
#include <string>
#include <utility>

#include "extensions/SourceInstall.h"
#include "extensions/Sources.h"

namespace genesis::extensions {

namespace {

// One segment of a namespaced id: lower-case letters, digits and hyphens.
// Duplicated from NativeManifest.cpp's local copy (and effects/Pack.cpp's):
// neither `extensions` nor `effects` is a dependency of the other, so the
// five-line predicate is repeated here rather than re-layered.
bool is_id_segment(std::string_view text)
{
    if (text.empty()) {
        return false;
    }
    for (const char c : text) {
        if (!((c >= 'a' && c <= 'z') || (c >= '0' && c <= '9') || c == '-')) {
            return false;
        }
    }
    return true;
}

// `author.name`: two segments of `is_id_segment`, split on the first dot.
bool is_namespaced_id(std::string_view id)
{
    const std::size_t dot = id.find('.');
    if (dot == std::string_view::npos) {
        return false;
    }
    return is_id_segment(id.substr(0, dot)) && is_id_segment(id.substr(dot + 1));
}

// Accumulates the entries and the problems found while reading the document. A
// problem recorded anywhere leaves the whole catalog unusable; the reader keeps
// going so a human sees every problem at once rather than only the first.
struct Builder
{
    std::vector<CatalogEntry> entries;
    std::vector<std::string> problems;

    void fail(std::string message) { problems.push_back(std::move(message)); }
};

// Reads one entry's `source` object: a `dir` or `git` type with a non-empty
// `url` (and, for `git`, optional `ref`/`subdir`). Returns false when the shape
// is unusable, leaving `out` untouched.
bool read_source(const nlohmann::json &entry, InstallSource &out, Builder &builder)
{
    const auto it = entry.find("source");
    if (it == entry.end() || !it->is_object()) {
        builder.fail("an entry has no source object");
        return false;
    }
    const nlohmann::json &source = *it;
    const auto type_it = source.find("type");
    if (type_it == source.end() || !type_it->is_string()) {
        builder.fail("a source has no `type`");
        return false;
    }
    const std::optional<SourceType> type = parse_source_type(type_it->get<std::string>());
    if (!type || (*type != SourceType::Dir && *type != SourceType::Git)) {
        builder.fail("a source `type` must be `dir` or `git`");
        return false;
    }
    const auto url_it = source.find("url");
    if (url_it == source.end() || !url_it->is_string() || url_it->get<std::string>().empty()) {
        builder.fail("a source must have a non-empty `url`");
        return false;
    }
    out.type = *type;
    out.url = url_it->get<std::string>();
    const auto ref_it = source.find("ref");
    if (ref_it != source.end() && ref_it->is_string()) {
        out.ref = ref_it->get<std::string>();
    }
    const auto subdir_it = source.find("subdir");
    if (subdir_it != source.end() && subdir_it->is_string()) {
        out.subdir = subdir_it->get<std::string>();
    }
    return true;
}

// Reads one entry. Returns false when it is unusable (a problem is recorded);
// on success the entry is appended.
void read_entry(const nlohmann::json &entry, Builder &builder)
{
    if (!entry.is_object()) {
        builder.fail("an entry is not an object");
        return;
    }
    const auto id_it = entry.find("id");
    if (id_it == entry.end() || !id_it->is_string()
        || !is_namespaced_id(id_it->get<std::string>())) {
        builder.fail("an entry's id is missing or not a namespaced id");
        return;
    }
    const auto name_it = entry.find("name");
    if (name_it == entry.end() || !name_it->is_string() || name_it->get<std::string>().empty()) {
        builder.fail("an entry's name is missing or empty");
        return;
    }

    CatalogEntry parsed;
    parsed.id = id_it->get<std::string>();
    parsed.name = name_it->get<std::string>();
    const auto version_it = entry.find("version");
    if (version_it != entry.end()) {
        if (version_it->is_string()) {
            parsed.version = version_it->get<std::string>();
        } else if (version_it->is_number_integer()) {
            parsed.version = std::to_string(version_it->get<std::int64_t>());
        }
    }
    const auto description_it = entry.find("description");
    if (description_it != entry.end() && description_it->is_string()) {
        parsed.description = description_it->get<std::string>();
    }
    const auto sha_it = entry.find("sha256");
    if (sha_it != entry.end() && sha_it->is_string()) {
        parsed.sha256 = sha_it->get<std::string>();
    }
    if (!read_source(entry, parsed.source, builder)) {
        return;
    }
    builder.entries.push_back(std::move(parsed));
}

} // namespace

CatalogResult parse_catalog(std::string_view text)
{
    Builder builder;
    nlohmann::json doc;
    try {
        doc = nlohmann::json::parse(text);
    } catch (const nlohmann::json::exception &error) {
        builder.fail(std::string("the catalog is not valid JSON: ") + error.what());
        return CatalogResult{ std::nullopt, std::move(builder.problems) };
    }

    if (!doc.is_object()) {
        builder.fail("the catalog is not a JSON object");
        return CatalogResult{ std::nullopt, std::move(builder.problems) };
    }

    std::uint32_t format = kCatalogFormat;
    const auto format_it = doc.find("format");
    if (format_it != doc.end()) {
        if (!format_it->is_number_integer() || format_it->get<std::int64_t>() < 0) {
            builder.fail("`format` must be a whole number");
            return CatalogResult{ std::nullopt, std::move(builder.problems) };
        }
        format = static_cast<std::uint32_t>(format_it->get<std::int64_t>());
    }
    if (format == 0) {
        builder.fail("format is 0; the first catalog format is 1");
        return CatalogResult{ std::nullopt, std::move(builder.problems) };
    }
    if (format > kCatalogFormat) {
        builder.fail("written for catalog format " + std::to_string(format)
                     + ", and this build reads up to " + std::to_string(kCatalogFormat));
        return CatalogResult{ std::nullopt, std::move(builder.problems) };
    }

    const auto entries_it = doc.find("entries");
    if (entries_it == doc.end() || !entries_it->is_array()) {
        builder.fail("`entries` is missing or not an array");
        return CatalogResult{ std::nullopt, std::move(builder.problems) };
    }
    for (const nlohmann::json &entry : *entries_it) {
        read_entry(entry, builder);
    }

    CatalogResult result;
    if (builder.problems.empty()) {
        result.entries = std::move(builder.entries);
    } else {
        result.problems = std::move(builder.problems);
    }
    return result;
}

CatalogResult fetch_catalog(const std::string &url, const CatalogFetcher &fetcher)
{
    CatalogResult result;
    if (!fetcher) {
        result.problems.push_back("no catalog fetcher is wired");
        return result;
    }
    const std::optional<std::string> body = fetcher(url);
    if (!body) {
        result.problems.push_back("could not fetch the catalog: " + url);
        return result;
    }
    return parse_catalog(*body);
}

InstallResult install_from_catalog(const std::string &url, const std::string &id,
                                   const std::filesystem::path &store_root, Origin origin,
                                   const CatalogFetcher &fetcher)
{
    const CatalogResult catalog = fetch_catalog(url, fetcher);
    if (!catalog.entries) {
        InstallResult result;
        result.problems = catalog.problems;
        return result;
    }
    for (const CatalogEntry &entry : *catalog.entries) {
        if (entry.id == id) {
            return install_from_source(entry.source, store_root, origin);
        }
    }
    InstallResult result;
    result.problems.push_back("unknown extension id '" + id + "' in the catalog");
    return result;
}

nlohmann::json catalog_entries_json(const std::vector<CatalogEntry> &entries)
{
    nlohmann::json out = nlohmann::json::array();
    for (const CatalogEntry &entry : entries) {
        nlohmann::json source = nlohmann::json::object();
        source["type"] = source_type_name(entry.source.type);
        source["url"] = entry.source.url;
        if (!entry.source.ref.empty()) {
            source["ref"] = entry.source.ref;
        }
        if (!entry.source.subdir.empty()) {
            source["subdir"] = entry.source.subdir;
        }

        nlohmann::json item = nlohmann::json::object();
        item["id"] = entry.id;
        item["name"] = entry.name;
        item["version"] = entry.version;
        item["description"] = entry.description;
        item["source"] = std::move(source);
        item["sha256"] = entry.sha256 ? nlohmann::json(*entry.sha256) : nlohmann::json(nullptr);
        out.push_back(std::move(item));
    }
    return out;
}

} // namespace genesis::extensions
