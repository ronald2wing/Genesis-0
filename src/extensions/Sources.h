// SPDX-License-Identifier: GPL-3.0-or-later
// SPDX-FileCopyrightText: 2026 Genesis-0 contributors

#pragma once

#include <filesystem>
#include <map>
#include <optional>
#include <string>

namespace genesis::extensions {

// Where an installed extension was recorded as coming from. The record exists
// so a config profile (see Config.h) can reproduce the install on another
// machine: the origin is the trust verdict, but the source is what a re-install
// would actually run. Unlike the origin record (a trust gate whose failure
// refuses the install), the source record is best-effort: a failed write still
// leaves a complete install, only without a reproducible source.
enum class SourceType { Dir, Git, Builtin };

// The install source recorded for one id. For `Dir` the `url` field holds the
// absolute path of the installed directory; for `Git` it holds the repository
// URL, with `ref` (the branch/tag/commit, empty for the default) and `subdir`
// (the path inside the repository holding the manifest, empty for the root)
// narrowing which tree to install; for `Builtin` all three are empty (a
// builtin re-seeds on startup, no source path to reproduce).
struct InstallSource
{
    SourceType type = SourceType::Dir;
    std::string url{ };
    std::string ref{ };
    std::string subdir{ };

    bool operator==(const InstallSource &) const = default;
};

// The source type spelled as the sources.json value, or its value read back.
const char *source_type_name(SourceType type);
std::optional<SourceType> parse_source_type(const std::string &text);

// The id -> InstallSource record read from `<store>/sources.json`, a flat
// `{"<id>": {"type": "dir"|"git"|"builtin", "url": ...}}` object. A `builtin`
// entry omits `url`; a `git` entry also carries `ref` and `subdir` when they
// are non-empty. An absent, unreadable or corrupt file reads as empty; an
// entry with an unknown type reads as `Dir` with an empty url (the
// conservative default).
std::map<std::string, InstallSource> read_sources(const std::filesystem::path &store_root);

// Records `source` for `id` and writes the map back atomically (a
// read-modify-write, so every other id's record survives). The latest record
// wins. Returns false on any I/O failure, in which case the on-disk record is
// unchanged.
bool record_source(const std::filesystem::path &store_root, const std::string &id,
                   InstallSource source);

} // namespace genesis::extensions
