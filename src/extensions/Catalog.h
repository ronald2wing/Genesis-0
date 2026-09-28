// SPDX-License-Identifier: GPL-3.0-or-later
// SPDX-FileCopyrightText: 2026 Genesis-0 contributors

#pragma once

#include <nlohmann/json.hpp>

#include <cstdint>
#include <filesystem>
#include <functional>
#include <optional>
#include <string>
#include <string_view>
#include <vector>

#include "extensions/Install.h"

namespace genesis::extensions {

// A marketplace catalog is a versioned JSON document listing installable
// extensions, each with the source that would reproduce it. It is the
// counterpart to a config profile (Config.h): a profile is the machine's own
// installed set, a catalog is a publisher's offer. Install trusts neither one
// more than the other - the same never-elevate-trust origin rules apply when an
// entry is installed.
//
// The catalog document is `{format: 1, entries: [...]}`; each entry is
// described beside `CatalogEntry`. The on-disk format is a versioned contract
// (format 1), so a newer format is refused rather than guessed.

// The catalog format version this build reads. A catalog with a higher
// `format` is refused (a newer build wrote it); a lower one still reads.
inline constexpr std::uint32_t kCatalogFormat = 1;

// One entry in a catalog: the identity a caller picks from, plus the source
// that would install it. `version` is a string carried verbatim from the
// document (a publisher may spell it how it likes); the install itself reads
// the version from the installed manifest, never from this field.
struct CatalogEntry
{
    std::string id;
    std::string name;
    std::string version;
    std::string description;
    InstallSource source;
    std::optional<std::string> sha256;
};

// What reading a catalog produced. `entries` is set only when the document
// parses and validates completely; any problem leaves it empty, so a caller
// can never half-consume a broken catalog. `problems` is empty on success.
struct CatalogResult
{
    std::optional<std::vector<CatalogEntry>> entries;
    std::vector<std::string> problems;
};

// Parses and validates a catalog document. Refused when the document is not an
// object, its `format` exceeds kCatalogFormat, `entries` is not an array, or an
// entry is malformed (a missing/blank/unnamespaced id, a missing name, or a
// source that is not a `dir`/`git` with a non-empty url).
CatalogResult parse_catalog(std::string_view text);

// A URL -> body fetch, injected so the host stays free of any HTTP client. An
// empty function means "no fetcher wired"; a returned nullopt means the fetch
// itself failed (the caller's error text).
using CatalogFetcher = std::function<std::optional<std::string>(const std::string &)>;

// Fetches and parses a catalog. Returns the entries, or (with `entries`
// empty) the reasons: no fetcher wired, the fetch failed, or the document did
// not parse/validate.
CatalogResult fetch_catalog(const std::string &url, const CatalogFetcher &fetcher);

// Fetches a catalog and installs the entry named `id`, following the entry's
// source through install_from_source (so a `git` entry clones, a `dir` entry
// installs the directory). The origin rules are install_from_source's: this
// never elevates trust. Returns the installed root, or the reasons (no fetcher,
// fetch/parse failure, unknown id, or a refused install).
InstallResult install_from_catalog(const std::string &url, const std::string &id,
                                   const std::filesystem::path &store_root, Origin origin,
                                   const CatalogFetcher &fetcher);

// The entries as the JSON array a caller lists (the API's `extensions.catalog`
// reply): each entry's id, name, version, description, source, and (when
// present) its sha256.
nlohmann::json catalog_entries_json(const std::vector<CatalogEntry> &entries);

} // namespace genesis::extensions
