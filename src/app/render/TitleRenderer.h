// SPDX-License-Identifier: GPL-3.0-or-later
// SPDX-FileCopyrightText: 2026 Genesis-0 contributors
//
// The title rasteriser: a TextStyle + text + canvas size become one frame-sized
// PNG, keyed so a title that has not changed is never painted twice.
//
// The layout is the host's, not this file's. `render::layout_title` places
// every word, and the rasteriser supplies QFontMetricsF through the Measure seam
// so the box each word is painted into is exactly the box the host laid out -
// the two cannot disagree because they share one arithmetic over one measure.
// The reveal map is `render::reveal_map_for` over the same measure, so a
// per-word reveal tracks the glyphs actually drawn.
//
// The cache reuses `render::MediaCache`'s freshness convention rather than
// inventing a second one: a `CacheKey` whose `source` carries a canonical
// serialisation of everything that changes the pixels, recorded beside the PNG
// by `cache_record` and compared by `cache_is_fresh`. Which `TextStyle` fields
// participate is documented on `title_cache_key`.

#pragma once

#include <QImage>

#include <cstdint>
#include <filesystem>
#include <string_view>
#include <vector>

#include "core/ShaderPass.h"
#include "project/model/Text.h"
#include "render/MediaCache.h"
#include "render/Titles.h"

namespace genesis::app {

// Everything one render produced. `image` is the frame-sized, premultiplied
// ARGB picture (fully transparent where nothing was drawn); `reveal` is the
// per-word order baked from the same boxes the words were painted into;
// `words` is those boxes in read order; `cached` says whether `image` was
// reused from the keyed cache rather than rasterised now.
struct RenderedTitle
{
    QImage image;
    genesis::core::RevealMap reveal;
    std::vector<genesis::render::WordBox> words;
    bool cached = false;
};

// Rasterises `style`+`text` at `canvas_width`×`canvas_height`, using the keyed
// cache under `cache_root` (empty = no cache, always rasterise). Runs on the
// calling thread and touches only Qt Gui locals and the filesystem, so it is
// safe off the UI thread. An unknown font family falls back to Qt's default
// rather than failing; empty text yields a fully transparent image, not an
// error.
RenderedTitle render_title(const genesis::project::TextStyle &style, std::string_view text,
                           std::uint32_t canvas_width, std::uint32_t canvas_height,
                           const std::filesystem::path &cache_root);

// The PNG a title spec maps to under `cache_root` (the exact cache file), and
// the key that records what produced it. Public so callers and tests can name
// the file and inspect the key.
std::filesystem::path title_cache_path(const std::filesystem::path &cache_root,
                                       const genesis::project::TextStyle &style,
                                       std::string_view text, std::uint32_t canvas_width,
                                       std::uint32_t canvas_height);
genesis::render::CacheKey title_cache_key(const genesis::project::TextStyle &style,
                                          std::string_view text, std::uint32_t canvas_width,
                                          std::uint32_t canvas_height);

} // namespace genesis::app
