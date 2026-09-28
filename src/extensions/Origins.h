// SPDX-License-Identifier: GPL-3.0-or-later
// SPDX-FileCopyrightText: 2026 Genesis-0 contributors

#pragma once

#include <filesystem>
#include <map>
#include <string>

#include "extensions/Trust.h"

namespace genesis::extensions {

// The origin named by a string, or Developer when the string is unknown (an
// unknown origin is treated as the conservative default: consent-gated).
Origin parse_origin(const std::string &text);

// The origin spelled as the origins.json value.
const char *origin_name(Origin origin);

// The id -> origin record every installed native extension's origin is read
// from (ExtensionHost::scan) and written to (install_extension). It persists
// as `<store>/origins.json`, a flat `{"<id>": "<origin>"}` object; an id absent
// from it is Developer, the historical default for a store entry. An unknown
// origin string reads as Developer, the conservative consent-gated default.
std::map<std::string, Origin> read_origins(const std::filesystem::path &store_root);

// Records `origin` for `id` and writes the map back atomically (a read-modify-
// write, so every other id's record survives). The latest record wins:
// recording an id that already has an origin overwrites it, which is what
// makes a reinstall/upgrade under a different origin take effect. Returns
// false on any I/O failure, in which case the on-disk record is unchanged.
bool record_origin(const std::filesystem::path &store_root, const std::string &id, Origin origin);

} // namespace genesis::extensions
