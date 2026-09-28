// SPDX-License-Identifier: GPL-3.0-or-later
// SPDX-FileCopyrightText: 2026 Genesis-0 contributors

#pragma once

#include <filesystem>
#include <optional>
#include <string_view>
#include <vector>

#include "render/ExportClip.h"

#include "project/Project.h"

// Flattening a project document into the exporter's clip list.
//
// Walking a timeline's clips, resolving their media and track, folding track
// state into per-clip flags, and building each clip's FFmpeg chains from the
// structured effects the document stores. An export is derived from the
// engine's own model, so the model of record and the rendered pixels cannot
// drift apart.
//
// Text clips are deliberately not flattened here. Titles rasterise
// separately and rejoin the exporter as plain image clips; see
// `ExportRequest`'s caller.

namespace genesis::render {

// Flattens one timeline of `project` for the exporter - the active one when
// `timeline_id` is absent. Text clips are excluded (they rasterise separately
// and rejoin as image clips); a clip whose media or track has vanished is
// skipped with the same tolerance the document reader extends.
std::vector<ExportClip> flatten_timeline(const genesis::project::Project &project,
                                         std::optional<std::string_view> timeline_id);

// `flatten_timeline`, knowing the mask root: a clip with a cutout is then told
// where its media's masks are, and carries the facts the engine needs to find
// and step them. Without the root the cutout is carried but not rendered.
std::vector<ExportClip>
flatten_timeline_in(const genesis::project::Project &project,
                    std::optional<std::string_view> timeline_id,
                    std::optional<std::filesystem::path> mask_root = std::nullopt);

// The constant the engine should receive for a keyable property.
//
// The clip's own, until that property is keyed - and then the *neutral*
// value, because a keyed property's keys travel absolutely rather than as a
// factor on this base. Absolute is not a preference: `Animation` multiplies
// the base by the scale and opacity tracks, so a clip sitting at zero
// opacity could never be keyed back up if its keys were relative to it.
double export_base(const genesis::project::Clip &clip, genesis::project::KeyProperty property);

// The keys the engine plays: the clip's own, property by property.
std::vector<ExportKey> export_keys(const genesis::project::Clip &clip);

} // namespace genesis::render
