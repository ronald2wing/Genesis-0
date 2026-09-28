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

#include "extensions/Sources.h"

namespace genesis::extensions {

// A config profile is the portable machine setup a user can carry between
// installs: the settings that are meaningful across machines, and the
// installed extensions with enough provenance (origin + source) to reproduce
// the install - or to decline it when it cannot be reproduced safely.
//
// The profile document is `{appVersion, settings, extensions}`; the `settings`
// object and the `extensions` array are described beside their structs below.
// There is exactly one profile format, so no version number is carried or
// read; an unknown key is ignored rather than refused.

// The portable settings subset. Every field is optional: a value only lands in
// an exported profile when the host's seam reports it, and on import an absent
// field is left untouched. Machine-specific paths (the proxy/cache root) are
// deliberately NOT carried - they are tied to the machine, not the user.
struct ConfigSettings
{
    std::optional<bool> proxies_enabled;
    std::optional<int> decode_preference; // 0 (auto), 1 (software), 2 (hardware)
    std::optional<int> scopes_interval_ms;
    std::optional<std::string> feed_url;
};

// The getter/setter seam the profile operations read settings through. Both
// directions are injected because the host-side library is Qt-free and cannot
// reach the app's controllers directly: the app wires lambdas over
// `AppSettings`/`UpdateController`, the CLI wires nothing (every setting is
// omitted on export and skipped on import). An absent getter means the setting
// is not exported; an absent setter means it cannot be applied on import.
struct ConfigSeam
{
    std::function<std::optional<bool>()> get_proxies_enabled;
    std::function<std::optional<int>()> get_decode_preference;
    std::function<std::optional<int>()> get_scopes_interval_ms;
    std::function<std::optional<std::string>()> get_feed_url;
    std::function<void(bool)> set_proxies_enabled;
    std::function<void(int)> set_decode_preference;
    std::function<void(int)> set_scopes_interval_ms;
    std::function<void(const std::string &)> set_feed_url;
};

// Exports the installed store plus the seam's settings as a profile document.
// For each installed id the highest version is exported, carrying the id's
// recorded origin (Developer when none) and its recorded install source (absent
// when none). `app_version` is the running build's version, recorded so an
// import can surface a version mismatch.
nlohmann::json export_profile(const std::filesystem::path &store_root, const ConfigSeam &seam,
                              std::string_view app_version);

// One extension's import outcome, for the report.
struct ImportedExtension
{
    std::string id;
    // "installed", "skipped" or "failed".
    std::string status;
    // Why a skipped/failed extension was not installed; empty for an install.
    std::string reason;
};

// What an import did. `settings` carries one human-readable line per setting
// (applied or skipped-with-reason); `extensions` carries one entry per
// profile extension.
struct ImportReport
{
    std::vector<std::string> settings;
    std::vector<ImportedExtension> extensions;
};

// Imports a profile document into `store_root`, applying the seam's settings
// and installing each listed extension per the never-elevate-trust rules:
//   - an id already installed (any version) is skipped;
//   - an entry with no recorded source is skipped (nothing to reproduce);
//   - a Builtin source is skipped (it re-seeds on startup);
//   - a Dir or Git source from a Developer origin installs (consent-gated) via
//     install_from_source, and fails when the path is gone or the clone fails;
//   - a Dir or Git source from a Curated/Builtin origin is skipped (a trusted
//     origin without a trusted source must not be re-trusted from a bare path);
//   - a User origin is failed (not installable).
// A profile that is not a JSON object is refused (an empty report is returned
// - see `import_profile` returning a nullopt for that case).
std::optional<ImportReport> import_profile(const nlohmann::json &profile,
                                           const std::filesystem::path &store_root,
                                           const ConfigSeam &seam);

} // namespace genesis::extensions
