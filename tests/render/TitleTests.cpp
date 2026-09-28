// SPDX-License-Identifier: GPL-3.0-or-later
// SPDX-FileCopyrightText: 2026 Genesis-0 contributors

#include <cmath>
#include <cstdint>
#include <cstdio>
#include <string>
#include <string_view>
#include <vector>

#include "render/Titles.h"

namespace {

int checks = 0;
int failures = 0;

void check(bool condition, const std::string &what)
{
    ++checks;
    if (!condition) {
        ++failures;
        std::printf("FAIL: %s\n", what.c_str());
    }
}

bool close(float a, float b, float eps = 1e-4f)
{
    return std::fabs(a - b) <= eps;
}

using genesis::project::TextAlign;
using genesis::project::TextStyle;
namespace render = genesis::render;

// A fake measure: every string is exactly W wide, a space included. Exact
// arithmetic, no font engine - the seam the layout is built around.
constexpr float W = 10.0f;
constexpr std::uint32_t CANVAS_W = 100;
constexpr std::uint32_t CANVAS_H = 100;

render::Measure constant(float width)
{
    return [width](std::string_view) { return width; };
}

void test_one_word()
{
    TextStyle style;
    style.align = TextAlign::Left;
    const render::TitleLayout layout =
            render::layout_title(style, "hi", CANVAS_W, CANVAS_H, constant(W));
    check(layout.words.size() == 1, "one word, one box");
    check(layout.lines.size() == 1, "one line");
    const render::WordBox &box = layout.words[0];
    check(box.x >= 0.0f && box.x + box.width <= CANVAS_W, "box inside the canvas");
    check(close(box.width, W), "box width is the measured width");
    check(close(box.x, 0.0f), "left-aligned starts at the left edge");
}

void test_three_words_read_order()
{
    TextStyle style;
    style.align = TextAlign::Left;
    const render::TitleLayout layout =
            render::layout_title(style, "one two three", CANVAS_W, CANVAS_H, constant(W));
    check(layout.words.size() == 3, "three boxes");
    const auto &words = layout.words;
    check(words[0].x < words[1].x && words[1].x < words[2].x, "left to right");
    check(close(words[0].y, words[1].y) && close(words[1].y, words[2].y), "all on one line");
    // Space is W too, so each box advances 2W from the last.
    check(close(words[0].x, 0.0f), "first word at the left edge");
    check(close(words[1].x, 2.0f * W), "second word after word + space");
    check(close(words[2].x, 4.0f * W), "third word after two gaps");
}

void test_wrap()
{
    TextStyle style;
    style.align = TextAlign::Left;
    // Five words fit (5*10 + 4*10 = 90 <= 100); the sixth does not, so it wraps.
    const render::TitleLayout layout =
            render::layout_title(style, "a b c d e f", CANVAS_W, CANVAS_H, constant(W));
    check(layout.words.size() == 6, "six words");
    check(layout.lines.size() == 2, "wraps onto a second line");
    check(close(layout.words[4].y, layout.words[0].y), "first five share line one");
    check(layout.words[5].y > layout.words[0].y, "line two sits below line one");
    check(close(layout.words[5].x, 0.0f), "wrapped word starts at the left edge");
}

void test_empty()
{
    TextStyle style;
    const render::TitleLayout layout =
            render::layout_title(style, "", CANVAS_W, CANVAS_H, constant(W));
    check(layout.words.empty(), "no boxes");
    check(layout.lines.empty(), "no lines");
    const genesis::core::RevealMap map =
            render::reveal_map_for(style, "", CANVAS_W, CANVAS_H, constant(W));
    check(map.width == CANVAS_W && map.height == CANVAS_H, "map sized to the canvas");
    bool all_revealed = true;
    for (const std::uint8_t value : *map.gray) {
        if (value != 0) {
            all_revealed = false;
        }
    }
    check(all_revealed, "every pixel revealed from the start");
}

void test_oversized_word_clamped()
{
    TextStyle style;
    style.align = TextAlign::Left;
    const render::TitleLayout layout = render::layout_title(
            style, "toolong", CANVAS_W, CANVAS_H, constant(3.0f * static_cast<float>(CANVAS_W)));
    check(layout.words.size() == 1, "one box");
    for (const render::WordBox &box : layout.words) {
        check(box.x >= 0.0f && box.x + box.width <= CANVAS_W + 1e-3f, "clamped inside the canvas");
    }
    check(close(layout.words[0].width, static_cast<float>(CANVAS_W)),
          "width clamped to the canvas");
}

void test_alignment()
{
    TextStyle left;
    left.align = TextAlign::Left;
    TextStyle center;
    center.align = TextAlign::Center;
    const render::TitleLayout l =
            render::layout_title(left, "word", CANVAS_W, CANVAS_H, constant(W));
    const render::TitleLayout c =
            render::layout_title(center, "word", CANVAS_W, CANVAS_H, constant(W));
    check(close(l.words[0].x, 0.0f), "left-aligned at the left edge");
    const float cx = c.words[0].x;
    check(close(cx, (CANVAS_W - W) / 2.0f), "centred x");
    check(!close(cx, l.words[0].x), "left and centre differ");
    const float right_gap = CANVAS_W - (cx + c.words[0].width);
    check(close(cx, right_gap), "centred box is symmetric");
}

void test_reveal_map_rect_count()
{
    TextStyle style;
    style.align = TextAlign::Left;
    const render::TitleLayout layout =
            render::layout_title(style, "one two three", CANVAS_W, CANVAS_H, constant(W));
    check(layout.words.size() == 3, "three words, three boxes");
    const genesis::core::RevealMap map =
            render::reveal_map_for(style, "one two three", CANVAS_W, CANVAS_H, constant(W));
    check(map.width == CANVAS_W && map.height == CANVAS_H, "map sized to the canvas");
    // from_rects spells a word's order as round(order / (n-1) * 255): the first
    // word is 0 (revealed from the start), so the three read-order words land as
    // 0, 128, 255 on the top scanline, one band each - one rect per word.
    check((*map.gray)[5] == 0, "first word's band is order 0");
    check((*map.gray)[25] == 128, "second word's band is order 1");
    check((*map.gray)[45] == 255, "third word's band is order 2");
}

} // namespace

int main()
{
    test_one_word();
    test_three_words_read_order();
    test_wrap();
    test_empty();
    test_oversized_word_clamped();
    test_alignment();
    test_reveal_map_rect_count();

    std::printf("%d checks, %d failures\n", checks, failures);
    return failures == 0 ? 0 : 1;
}
