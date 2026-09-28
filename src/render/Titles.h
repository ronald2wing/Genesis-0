// SPDX-License-Identifier: GPL-3.0-or-later
// SPDX-FileCopyrightText: 2026 Genesis-0 contributors

#pragma once

#include <cstdint>
#include <functional>
#include <string>
#include <string_view>
#include <vector>

#include "core/ShaderPass.h"
#include "project/model/Text.h"

// Host-side title layout: where a title's words sit on the canvas, and the
// per-word reveal map built from those boxes.
//
// The host has no font engine and must not grow one: the caller supplies the
// one number the host cannot derive - the advance width of a run of text - and
// everything here is deterministic arithmetic over that. No time, no
// randomness, no locale.
//
// Width model: `font_size` is a fraction of frame height, so the title's font
// size is `font_size * canvas_height` pixels. A line is `line_height *
// font_size * canvas_height` pixels tall. The gap between two words on a line
// is the measured width of a single space, `measure(" ")`. Each word's box is
// its measured width by one line height, the height chosen to cover a glyph's
// full ascent and descent since only advance width is known. Lines stack from
// the top of the canvas (y = 0); vertical placement is the caller's transform,
// as `TextStyle` carries no vertical anchor.

namespace genesis::render {

// Where one word sits on the canvas, in pixels, in read order. The reveal map
// is built from exactly these, so the order here is the order words appear.
struct WordBox
{
    float x = 0;
    float y = 0;
    float width = 0;
    float height = 0;
};

// The measured advance width of `text` at the title's font, in pixels. The host
// has no font engine and must not grow one: the caller supplies real metrics
// (a Qt renderer uses QFontMetricsF) and a test supplies a fake. This seam is
// why the layout is testable without a screen.
using Measure = std::function<float(std::string_view text)>;

// One laid-out title: the wrapped lines and the box of every word in read order.
// Empty text yields no boxes, not a failure.
struct TitleLayout
{
    std::vector<std::string> lines;
    std::vector<WordBox> words;
    bool operator==(const TitleLayout &) const = default;
};

// Lays the text out inside the canvas, honouring the style's alignment. Wraps on
// spaces when a line would exceed the canvas, and clamps a single word wider than
// the canvas rather than letting it overflow.
TitleLayout layout_title(const genesis::project::TextStyle &style, std::string_view text,
                         std::uint32_t canvas_width, std::uint32_t canvas_height,
                         const Measure &measure);

// The reveal map for the same layout: one rect per word box, in read order.
// Empty text gives the map that reveals everything.
genesis::core::RevealMap reveal_map_for(const genesis::project::TextStyle &style,
                                        std::string_view text, std::uint32_t canvas_width,
                                        std::uint32_t canvas_height, const Measure &measure);

} // namespace genesis::render
