// SPDX-License-Identifier: GPL-3.0-or-later
// SPDX-FileCopyrightText: 2026 Genesis-0 contributors

#include <cmath>
#include <cstdio>
#include <filesystem>
#include <fstream>
#include <optional>
#include <string>
#include <system_error>
#include <vector>

#include "../support/Checks.h"
#include "render/TitleTemplates.h"

namespace {

using genesis::test::check;
using genesis::test::checks;
using genesis::test::failures;
bool close(double a, double b, double eps = 1e-6)
{
    return std::fabs(a - b) <= eps;
}

namespace render = genesis::render;
using genesis::project::TextAlign;
using genesis::project::TextStyle;

// The assets/titles/ directory, found by walking up from the working
// directory - the same walk the catalogue and pack-source tests use, because
// the test binary runs from anywhere under the tree.
std::optional<std::filesystem::path> locate_titles()
{
    std::filesystem::path dir = std::filesystem::current_path();
    while (true) {
        const std::filesystem::path candidate = dir / "assets" / "titles";
        std::error_code ec;
        if (std::filesystem::is_directory(candidate, ec) && !ec) {
            return candidate;
        }
        const std::filesystem::path parent = dir.parent_path();
        if (parent == dir) {
            return std::nullopt;
        }
        dir = parent;
    }
}

// A scratch directory for fixtures, created once per run. A fixed name is
// enough for a standalone test; files inside are overwritten on each run.
std::filesystem::path scratch_dir()
{
    const std::filesystem::path dir =
            std::filesystem::temp_directory_path() / "genesis_title_templates_tests";
    std::error_code ec;
    std::filesystem::create_directories(dir, ec);
    return dir;
}

void write_file(const std::filesystem::path &path, const std::string &content)
{
    std::ofstream out(path, std::ios::binary);
    out << content;
    check(out.good(), "fixture written: " + path.string());
}

// A hand-written fixture exercising every attribute the extractor supports:
// style="..." declarations and presentation attributes, a family in both
// unquoted and single-quoted spelling, font-size on the tspan and on the text
// element, bold/italic weight/style, hex fills, and text-anchor.
const char *fixture_svg = R"(<?xml version="1.0" encoding="UTF-8"?>
<svg xmlns="http://www.w3.org/2000/svg" width="1920" height="1080">
  <text x="960" y="500" text-anchor="middle"
        style="font-family:DejaVu Sans;font-weight:bold;font-style:italic;fill:#ff0000">
    <tspan style="font-size:108px">The Title</tspan>
  </text>
  <text x="960" y="640" text-anchor="middle"
        font-family="'DejaVu Sans'" font-size="54px" font-weight="bold" fill="#00ff00">
    <tspan>Sub-Title</tspan>
  </text>
</svg>
)";

void test_fixture_extracts_placeholder_and_style()
{
    const std::filesystem::path fixture = scratch_dir() / "fixture.svg";
    write_file(fixture, fixture_svg);

    const render::ParseResult parsed = render::parse_template(fixture);
    check(parsed.template_.has_value(), "fixture parses");
    check(parsed.reason.empty(), "no reason on success");
    if (!parsed.template_) {
        return;
    }
    const render::TitleTemplate &tmpl = *parsed.template_;
    check(tmpl.name == "fixture", "name is the stem");
    check(tmpl.path == fixture, "path is the file parsed");
    check(tmpl.text_slots.size() == 2, "two <text> elements, two slots");

    if (tmpl.text_slots.size() != 2) {
        return;
    }
    const render::TitleSlot &title = tmpl.text_slots[0];
    check(title.index == 0, "first slot is index 0");
    check(title.placeholder == "The Title", "title placeholder extracted");
    check(title.style.font_family == "\"DejaVu Sans\"", "unquoted family is quoted");
    check(close(title.style.font_size, 108.0 / 1080.0),
          "tspan font-size scaled by the 1080 reference height");
    check(close(title.style.font_weight, 700.0), "bold reads as 700");
    check(title.style.italic, "italic style flag read");
    check(title.style.color == "#ff0000", "hex fill becomes the colour");
    check(title.style.align == TextAlign::Center, "middle anchor reads centred");

    const render::TitleSlot &subtitle = tmpl.text_slots[1];
    check(subtitle.index == 1, "second slot is index 1");
    check(subtitle.placeholder == "Sub-Title", "subtitle placeholder extracted");
    check(subtitle.style.font_family == "\"DejaVu Sans\"", "single-quoted family is re-quoted");
    check(close(subtitle.style.font_size, 54.0 / 1080.0), "presentation font-size scaled");
    check(close(subtitle.style.font_weight, 700.0), "presentation bold reads as 700");
    check(!subtitle.style.italic, "no italic when unstated");
    check(subtitle.style.color == "#00ff00", "presentation hex fill read");
    check(subtitle.style.align == TextAlign::Center, "presentation middle anchor reads centred");
}

void test_fill_slots_substitutes_and_preserves_style()
{
    const std::filesystem::path fixture = scratch_dir() / "fixture.svg";
    write_file(fixture, fixture_svg);
    const render::ParseResult parsed = render::parse_template(fixture);
    check(parsed.template_.has_value(), "fixture parses for fill");
    if (!parsed.template_) {
        return;
    }
    const std::vector<std::pair<std::string, TextStyle>> filled =
            render::fill_slots(*parsed.template_, { "Hello", "World" });
    check(filled.size() == 2, "one pair per slot");
    if (filled.size() != 2) {
        return;
    }
    check(filled[0].first == "Hello", "user text replaces the placeholder");
    check(filled[1].first == "World", "second slot filled in order");
    check(filled[0].second == parsed.template_->text_slots[0].style,
          "style carried through unchanged");
    check(filled[1].second == parsed.template_->text_slots[1].style,
          "second slot's style carried through unchanged");

    // A short text list leaves the unfilled slot's placeholder in place.
    const std::vector<std::pair<std::string, TextStyle>> partial =
            render::fill_slots(*parsed.template_, { "Solo" });
    check(partial.size() == 2, "a short list still fills every slot");
    if (partial.size() == 2) {
        check(partial[0].first == "Solo", "given text lands in its slot");
        check(partial[1].first == "Sub-Title", "missing text keeps the placeholder");
    }
}

void test_empty_and_malformed_are_refused_with_reason()
{
    const std::filesystem::path empty = scratch_dir() / "empty.svg";
    write_file(empty, "");
    const render::ParseResult no_bytes = render::parse_template(empty);
    check(!no_bytes.template_.has_value(), "an empty file is refused");
    check(!no_bytes.reason.empty(), "a reason names the refusal");
    check(no_bytes.reason == "no <text> elements", "the reason is textless, not a crash");

    const std::filesystem::path no_text = scratch_dir() / "no_text.svg";
    write_file(no_text, "<svg width=\"1920\" height=\"1080\"><rect/></svg>\n");
    const render::ParseResult no_slots = render::parse_template(no_text);
    check(!no_slots.template_.has_value(), "an SVG without <text> is refused");
    check(!no_slots.reason.empty(), "textless refusal carries a reason");

    const std::filesystem::path missing = scratch_dir() / "does_not_exist.svg";
    const render::ParseResult absent = render::parse_template(missing);
    check(!absent.template_.has_value(), "a missing file is refused");
    check(absent.reason == "cannot read file", "an unreadable file names the cause");

    const std::filesystem::path truncated = scratch_dir() / "truncated.svg";
    write_file(truncated, "<svg><text>no close");
    const render::ParseResult cut = render::parse_template(truncated);
    check(!cut.template_.has_value(), "an unterminated <text> is refused");
    check(!cut.reason.empty(), "unterminated refusal carries a reason");
}

void test_gradient_fill_is_dropped_not_guessed()
{
    const std::filesystem::path gradient = scratch_dir() / "gradient.svg";
    write_file(gradient,
               "<svg xmlns=\"http://www.w3.org/2000/svg\" width=\"1920\" "
               "height=\"1080\">\n"
               "  <text x=\"960\" y=\"500\" fill=\"url(#linearGradient1)\" "
               "font-size=\"72px\">\n"
               "    <tspan>The Title</tspan>\n"
               "  </text>\n"
               "</svg>\n");
    const render::ParseResult parsed = render::parse_template(gradient);
    check(parsed.template_.has_value(), "a gradient-filled template still parses");
    if (!parsed.template_ || parsed.template_->text_slots.empty()) {
        return;
    }
    const TextStyle style = parsed.template_->text_slots[0].style;
    check(style.color == "#ffffff",
          "a gradient fill is dropped to the default colour, not guessed");
    check(close(style.font_size, 72.0 / 1080.0), "the size still extracts beside the dropped fill");
}

void test_discovery_counts_shipped_templates()
{
    const std::optional<std::filesystem::path> root = locate_titles();
    if (!root) {
        check(false, "assets/titles/ found by walking up from cwd");
        return;
    }
    const render::TemplateLibrary library = render::discover_templates(*root);

    // The shipped set: every *.svg under assets/titles. The count is asserted
    // so a broken or silently-trimmed addition fails loudly rather than
    // quietly shrinking the library.
    constexpr std::size_t SHIPPED_TEMPLATES = 20;
    check(library.templates.size() == SHIPPED_TEMPLATES, "every shipped template is discovered");
    check(library.refusals.empty(), "no shipped template is refused");

    std::size_t total_slots = 0;
    for (const render::TitleTemplate &tmpl : library.templates) {
        if (tmpl.text_slots.empty()) {
            std::printf("FAIL: %s yields no slot\n", tmpl.name.c_str());
            ++checks;
            ++failures;
        }
        total_slots += tmpl.text_slots.size();
    }
    // The sum of the shipped templates' <text> elements: a template whose text
    // stopped extracting, or a template added/removed, changes this.
    constexpr std::size_t SHIPPED_SLOTS = 47;
    check(total_slots == SHIPPED_SLOTS, "total slots over the shipped set");
}

} // namespace

int main()
{
    test_fixture_extracts_placeholder_and_style();
    test_fill_slots_substitutes_and_preserves_style();
    test_empty_and_malformed_are_refused_with_reason();
    test_gradient_fill_is_dropped_not_guessed();
    test_discovery_counts_shipped_templates();

    return genesis::test::summary();
}
