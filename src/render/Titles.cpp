// SPDX-License-Identifier: GPL-3.0-or-later
// SPDX-FileCopyrightText: 2026 Genesis-0 contributors

#include "render/Titles.h"

#include <algorithm>
#include <array>
#include <cctype>
#include <cmath>
#include <cstddef>
#include <cstdint>
#include <string>
#include <string_view>
#include <vector>

namespace genesis::render {

namespace {

// One word of the title, its measured advance width, and whether a hard line
// break (a newline in the source text) must come before it.
struct Piece
{
    std::string word;
    float width;
    bool forced;
};

// Splits `text` into words on whitespace. A newline is a hard break: it ends
// the current line rather than just separating two words on one. Runs of
// whitespace collapse, so the layout does not care how much air the source
// left between words.
std::vector<Piece> tokenise(std::string_view text, const Measure &measure)
{
    std::vector<Piece> pieces;
    std::string current;
    bool break_before = false;
    const auto finish = [&]() {
        if (current.empty()) {
            return;
        }
        pieces.push_back(Piece{ current, measure(current), break_before });
        current.clear();
        break_before = false;
    };
    for (const char c : text) {
        if (c == '\n') {
            finish();
            break_before = true;
        } else if (std::isspace(static_cast<unsigned char>(c))) {
            finish();
        } else {
            current.push_back(c);
        }
    }
    finish();
    return pieces;
}

} // namespace

TitleLayout layout_title(const genesis::project::TextStyle &style, std::string_view text,
                         std::uint32_t canvas_width, std::uint32_t canvas_height,
                         const Measure &measure)
{
    TitleLayout layout;
    const float canvas_w = static_cast<float>(canvas_width);
    const float canvas_h = static_cast<float>(canvas_height);
    const float line_h =
            static_cast<float>(style.line_height) * static_cast<float>(style.font_size) * canvas_h;
    const float space_w = measure(" ");

    const std::vector<Piece> pieces = tokenise(text, measure);

    std::vector<Piece> line;
    float line_w = 0.0F;
    float y = 0.0F;

    const auto flush = [&]() {
        if (line.empty()) {
            return;
        }
        const float width = line_w;
        float x = 0.0F;
        if (style.align == genesis::project::TextAlign::Center) {
            x = (canvas_w - width) * 0.5F;
        } else if (style.align == genesis::project::TextAlign::Right) {
            x = canvas_w - width;
        }
        std::string joined;
        for (const Piece &piece : line) {
            const float w = std::min(piece.width, canvas_w);
            const float bx = std::clamp(x, 0.0F, canvas_w - w);
            layout.words.push_back(WordBox{ bx, y, w, line_h });
            x += piece.width + space_w;
            if (!joined.empty()) {
                joined.push_back(' ');
            }
            joined += piece.word;
        }
        layout.lines.push_back(std::move(joined));
        y += line_h;
        line.clear();
        line_w = 0.0F;
    };

    for (const Piece &piece : pieces) {
        if (piece.forced && !line.empty()) {
            flush();
        }
        if (!line.empty() && line_w + space_w + piece.width > canvas_w) {
            flush();
        }
        line_w += (line.empty() ? 0.0F : space_w) + piece.width;
        line.push_back(piece);
    }
    flush();

    return layout;
}

genesis::core::RevealMap reveal_map_for(const genesis::project::TextStyle &style,
                                        std::string_view text, std::uint32_t canvas_width,
                                        std::uint32_t canvas_height, const Measure &measure)
{
    const TitleLayout layout = layout_title(style, text, canvas_width, canvas_height, measure);
    std::vector<std::array<std::int32_t, 4>> rects;
    rects.reserve(layout.words.size());
    for (const WordBox &word : layout.words) {
        rects.push_back(std::array<std::int32_t, 4>{
                static_cast<std::int32_t>(std::lround(word.x)),
                static_cast<std::int32_t>(std::lround(word.y)),
                static_cast<std::int32_t>(std::lround(word.width)),
                static_cast<std::int32_t>(std::lround(word.height)),
        });
    }
    return genesis::core::RevealMap::from_rects(canvas_width, canvas_height, rects);
}

} // namespace genesis::render
