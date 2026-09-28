// SPDX-License-Identifier: GPL-3.0-or-later
// SPDX-FileCopyrightText: 2026 Genesis-0 contributors
//
// Text model: a title's alignment and styling.

#pragma once

#include <algorithm>
#include <cmath>
#include <string>

namespace genesis::project {

// How a title's lines sit within their block, and which point of the block
// the clip's position pins: a centred title is placed by its middle, a
// left-aligned one by its block's left edge and a right-aligned one by its
// right edge. Typing more into a left-aligned title grows it to the right
// and leaves its left edge where it was, as alignment means everywhere.
// On disk: lowercase.
enum class TextAlign {
    // Lines share a left edge, which is where the clip's position is.
    Left,
    // Lines are centred on each other and on the clip's position - the
    // default, and what the reader falls back to for an unrecognised value.
    Center,
    // Lines share a right edge, which is where the clip's position is.
    Right,
};

// A title's styling. Sizes are fractions of the frame, so a title composed
// against 1080p lands correctly exported at 4K. A style arriving in a
// command needs only the fields it changes; the rest are the default's, so
// a caller can say `{ "content": "Hello" }` and get a title that looks like
// one the window would place.
// On disk: camelCase.
struct TextStyle
{
    // The words themselves, newlines included. The clip's display name is
    // snapshotted from the first non-empty line.
    std::string content = "Your text";
    // CSS-style family name, quotes included where the name needs them,
    // e.g. `"Cabinet Grotesk"`. May name a `CustomFont` the user added.
    std::string font_family = "\"Cabinet Grotesk\"";
    // Cap height as a fraction of frame height.
    double font_size = 0.09;
    // CSS-scale weight, 100..=900; the reader clamps hand-edited values
    // into that range.
    double font_weight = 700.0;
    // Italic when true. A style flag, not a separate face.
    bool italic = false;
    // Fill colour as a CSS hex string, e.g. "#ffffff".
    std::string color = "#ffffff";
    // Line alignment within the block; see `TextAlign`. Read with
    // tolerance: an unrecognised value reads as centred.
    TextAlign align = TextAlign::Center;
    // Opacity of the whole title in 0..=1, multiplied with the clip's own
    // opacity.
    double opacity = 1.0;
    // Outline thickness as a fraction of frame height. Zero - the default -
    // means no stroke.
    double stroke_width = 0.0;
    // Outline colour, only visible when `stroke_width` is non-zero.
    std::string stroke_color = "#000000";
    // A drop shadow for legibility over footage. On by default.
    bool shadow = true;
    // A solid background behind the text, `#rrggbb[aa]`; its alpha is the
    // background's opacity. Empty string for none. The painter and older
    // comments call it the plate.
    std::string background;
    // The background's corner radius as a fraction of frame height, so a
    // title composed against 1080p keeps its corners at 4K. The default is
    // the rounding every background wore before it had a dial - 15 % of
    // the default size's em - and zero is square.
    double background_radius = 0.0135;
    // The air between the words and the background's left and right
    // edges, as a fraction of frame height, on an axis the words size
    // themselves: a box with a width is exactly that wide. The default
    // is the air every background had before it was a dial, 35 % of the
    // default size's em.
    double background_padding_x = 0.0315;
    // The same above and below; 20 % of the default em by default.
    double background_padding_y = 0.018;
    // Baseline spacing as a multiple of the font size; the reader floors it
    // at 0.5 so lines cannot collapse onto each other.
    double line_height = 1.2;
    // Extra letter spacing, in the same frame-height fractions as
    // `font_size`. Zero is the font's natural fit.
    double tracking = 0.0;
    // The widest a line may run, as a fraction of frame width, before its
    // words wrap onto the next; the stage's side grips set it. Zero - the
    // default - is no limit: a line is as long as its words. With a limit
    // the block is a box exactly that wide, plate included, and the lines
    // align inside it.
    double max_width = 0.0;
    // The box's height as a fraction of frame height; the stage's top and
    // bottom grips set it, and the inspector's Height field. Zero - the
    // default - is the words' own height. With a height the block is a
    // box exactly that tall, the words centred in it, so a lower third
    // can be a band of a fixed size whatever is written on it.
    double max_height = 0.0;

    TextStyle() = default;

    bool operator==(const TextStyle &) const = default;

    // Every number pulled into the range a title can be drawn at: a
    // hand-edited zero size would render an invisible title, and lines
    // cannot collapse onto each other.
    TextStyle tidy() const
    {
        const auto finite = [](double value, double fallback) {
            return std::isfinite(value) ? value : fallback;
        };
        TextStyle out = *this;
        const TextStyle base{ };
        out.font_size = std::clamp(finite(out.font_size, base.font_size), 0.01, 1.0);
        out.font_weight = std::clamp(finite(out.font_weight, base.font_weight), 100.0, 900.0);
        out.opacity = std::clamp(finite(out.opacity, base.opacity), 0.0, 1.0);
        out.stroke_width = std::max(finite(out.stroke_width, base.stroke_width), 0.0);
        out.line_height = std::max(finite(out.line_height, base.line_height), 0.5);
        out.tracking = finite(out.tracking, base.tracking);
        out.max_width = std::max(finite(out.max_width, base.max_width), 0.0);
        out.max_height = std::max(finite(out.max_height, base.max_height), 0.0);
        out.background_radius =
                std::max(finite(out.background_radius, base.background_radius), 0.0);
        out.background_padding_x =
                std::max(finite(out.background_padding_x, base.background_padding_x), 0.0);
        out.background_padding_y =
                std::max(finite(out.background_padding_y, base.background_padding_y), 0.0);
        return out;
    }
};

} // namespace genesis::project
