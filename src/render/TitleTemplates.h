// SPDX-License-Identifier: GPL-3.0-or-later
// SPDX-FileCopyrightText: 2026 Genesis-0 contributors

#pragma once

#include <cstddef>
#include <filesystem>
#include <optional>
#include <string>
#include <utility>
#include <vector>

#include "project/model/Text.h"

// The title template library: the in-house SVG artwork (see
// assets/titles/README.md), read for the text slots a user edits. This is
// extraction, not rendering - the SVG artwork (rects, gradients, filters) is
// never rasterised here, and Qt is deliberately not involved. The point is to
// hand the host's existing title pipeline - layout_title and
// TitleRenderer::render, which consume a (TextStyle, text) pair - the
// placeholder text and the styling it can express, so a filled template draws
// through the same arithmetic as a hand-typed title.
//
// The extraction is deliberately narrow: each SVG <text> element becomes one
// slot, and only the attributes TextStyle can express are read (font-family,
// font-size, font-weight, font-style, fill, text-anchor). Everything else -
// positions, transforms, strokes, gradient fills, line spacing - is dropped;
// the README lists what is lost. The scanner finds literal <text> tags and is
// tuned to these templates, not a general SVG/XML parser.

namespace genesis::render {

// One editable text slot extracted from a template's <text> elements, in
// document order.
struct TitleSlot
{
    // The slot's 0-based position among the SVG's <text> elements.
    std::size_t index = 0;
    // The placeholder text the artwork carries, e.g. "The Title".
    std::string placeholder;
    // The styling TextStyle can express, mapped from the element's attributes.
    genesis::project::TextStyle style;

    bool operator==(const TitleSlot &) const = default;
};

// One title template: the SVG file plus its editable text slots.
struct TitleTemplate
{
    // The file's basename without the .svg extension.
    std::string name;
    // The SVG file itself, as discovered on disk.
    std::filesystem::path path;
    // The editable text slots, in document order. Never empty: a file with no
    // <text> element is refused, not parsed to an empty template.
    std::vector<TitleSlot> text_slots;

    bool operator==(const TitleTemplate &) const = default;
};

// A template file that could not be parsed, and why.
struct TemplateRefusal
{
    std::filesystem::path path;
    // Why the file was refused - unreadable, or it held no <text> element.
    std::string reason;

    bool operator==(const TemplateRefusal &) const = default;
};

// What discover_templates found: the parsed templates (sorted by name) and
// the files refused, reported rather than silently skipped.
struct TemplateLibrary
{
    std::vector<TitleTemplate> templates;
    std::vector<TemplateRefusal> refusals;
};

// The outcome of parsing one SVG file. `template_` is set on success; on
// failure it is nullopt and `reason` says why (unreadable, or no <text>).
struct ParseResult
{
    std::optional<TitleTemplate> template_;
    std::string reason;
};

// Parses one SVG file into a template. A template is only the text slots; the
// artwork is never rasterised. Refuses (nullopt + reason) a file that cannot
// be read or holds no <text> element.
ParseResult parse_template(const std::filesystem::path &path);

// Discovers every *.svg under `directory`, sorted by name, parsing each.
// Unreadable or textless files land in `refusals` with their reason, not in
// `templates`.
TemplateLibrary discover_templates(const std::filesystem::path &directory);

// Fills a template's slots with user text: slot i's placeholder is replaced by
// texts[i]; a missing entry keeps the placeholder in place. The result is
// (text, style) pairs in slot order - exactly what layout_title and
// TitleRenderer::render consume, one pair per slot so each keeps its own
// styling.
std::vector<std::pair<std::string, genesis::project::TextStyle>>
fill_slots(const TitleTemplate &tmpl, const std::vector<std::string> &texts);

} // namespace genesis::render
