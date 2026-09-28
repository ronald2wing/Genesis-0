// SPDX-License-Identifier: GPL-3.0-or-later
// SPDX-FileCopyrightText: 2026 Genesis-0 contributors
//
// Ported from Concat's concat-project/src/doc.rs,
// SPDX-License-Identifier: GPL-3.0-or-later,
// SPDX-FileCopyrightText: 2026 Jareer and Concat contributors.
//
// Internal document codec surface: the cross-file functions shared between the
// split Read/Write/Migrate/Settle translation units. Not for callers outside
// the document codec.

#pragma once

#include <nlohmann/json.hpp>
#include <optional>

#include "project/Project.h"

namespace genesis::project::detail {

using nlohmann::json;

// -- struct writers ---------------------------------------------------------

json write_media_item(const MediaItem &item);
json write_track(const Track &track);
json write_clip(const Clip &clip);
json write_font(const CustomFont &font);
json write_timeline(const Timeline &timeline);

// -- struct readers ---------------------------------------------------------

std::optional<MediaItem> read_media_item(const json &j);
std::optional<CustomFont> read_font(const json &j);
std::optional<Timeline> read_timeline(const json &j);

// -- migrations -------------------------------------------------------------

std::optional<json> migrate(const json &doc);

// -- parse and settle -------------------------------------------------------

std::optional<Project> parse_project(const json &j);
std::optional<Project> settle_project(Project project);

} // namespace genesis::project::detail
