// SPDX-License-Identifier: GPL-3.0-or-later
// SPDX-FileCopyrightText: 2026 Genesis-0 contributors

#pragma once

#include <filesystem>
#include <optional>

#include "render/Resolve.h"

namespace genesis::project {
struct Project;
}

// Flattens and resolves the active timeline into the engine's form, then
// rasterises its titles into the cache so the adapter can place them. One
// place for the demo's frame request and title map, so the initial build and
// every post-edit reload stay on the same settings rather than drifting apart.
// `cache_root` holds the title PNGs and the reverse copies; `proxy_cache_root`
// is the folder the app generated (or is generating) whole-source downscales
// into - when set, a forward-playing video clip with a fresh proxy reads it
// instead of the original. Export callers pass nothing here, so export always
// reads originals. `mask_root` is the folder the masks live under; when set, a
// cutout clip carries its mask spec into the engine and renders cut.
genesis::render::BuiltTimeline
build_for_engine(const genesis::project::Project &project, const std::filesystem::path &cache_root,
                 std::optional<std::filesystem::path> proxy_cache_root = std::nullopt,
                 std::optional<std::filesystem::path> mask_root = std::nullopt,
                 const genesis::render::Catalogue *catalogue = nullptr);
