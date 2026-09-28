// SPDX-License-Identifier: GPL-3.0-or-later
// SPDX-FileCopyrightText: 2026 Genesis-0 contributors

#include "render/TitleTemplates.h"

#include <algorithm>
#include <cctype>
#include <charconv>
#include <fstream>
#include <map>
#include <sstream>
#include <string>
#include <string_view>
#include <system_error>

namespace genesis::render {

namespace {

// The reference frame height every in-house title template is authored at
// (see assets/titles/README.md).
// TextStyle::font_size is a fraction of frame height, so an SVG font-size in
// pixels is divided by this to land in the host's units.
constexpr double REFERENCE_CANVAS_HEIGHT = 1080.0;

// A character that can keep an SVG tag/attribute name going, so `<text` is not
// confused with `<textPath` or `<textarea`.
bool is_name_char(char c)
{
    return std::isalnum(static_cast<unsigned char>(c)) != 0 || c == '-' || c == ':' || c == '_';
}

// ASCII whitespace trimmed from both ends.
std::string_view trim(std::string_view s)
{
    std::size_t begin = 0;
    while (begin < s.size() && std::isspace(static_cast<unsigned char>(s[begin])) != 0) {
        ++begin;
    }
    std::size_t end = s.size();
    while (end > begin && std::isspace(static_cast<unsigned char>(s[end - 1])) != 0) {
        --end;
    }
    return s.substr(begin, end - begin);
}

// Decodes the five predefined XML character references. The templates'
// placeholder text is plain ASCII, so this is completeness-on-principle rather
// than
// load-bearing; an unknown reference is kept verbatim.
std::string decode_entities(std::string_view s)
{
    std::string out;
    out.reserve(s.size());
    for (std::size_t i = 0; i < s.size();) {
        if (s[i] != '&') {
            out.push_back(s[i]);
            ++i;
            continue;
        }
        const std::size_t semi = s.find(';', i + 1);
        if (semi == std::string_view::npos) {
            out.push_back('&');
            ++i;
            continue;
        }
        const std::string_view entity = s.substr(i + 1, semi - i - 1);
        char decoded = '\0';
        if (entity == "amp")
            decoded = '&';
        if (entity == "lt")
            decoded = '<';
        if (entity == "gt")
            decoded = '>';
        if (entity == "quot")
            decoded = '"';
        if (entity == "apos")
            decoded = '\'';
        if (decoded != '\0') {
            out.push_back(decoded);
            i = semi + 1;
        } else {
            // Unknown reference: keep the '&' and let the rest scan as text.
            out.push_back('&');
            ++i;
        }
    }
    return out;
}

// The value of the attribute `name` in a tag, or nullopt. `tag` is an opening
// tag with or without its angle brackets; values may be single- or
// double-quoted. `name` must sit at a word boundary, so asking for "fill" does
// not match "fill-opacity".
std::optional<std::string> attribute_value(std::string_view tag, std::string_view name)
{
    std::size_t pos = 0;
    while (pos <= tag.size()) {
        const std::size_t found = tag.find(name, pos);
        if (found == std::string_view::npos) {
            return std::nullopt;
        }
        if (found > 0 && is_name_char(tag[found - 1])) {
            pos = found + name.size();
            continue;
        }
        std::size_t eq = found + name.size();
        while (eq < tag.size() && std::isspace(static_cast<unsigned char>(tag[eq])) != 0) {
            ++eq;
        }
        if (eq >= tag.size() || tag[eq] != '=') {
            pos = found + name.size();
            continue;
        }
        std::size_t start = eq + 1;
        while (start < tag.size() && std::isspace(static_cast<unsigned char>(tag[start])) != 0) {
            ++start;
        }
        if (start >= tag.size()) {
            return std::nullopt;
        }
        const char quote = tag[start];
        if (quote != '"' && quote != '\'') {
            return std::nullopt;
        }
        const std::size_t end = tag.find(quote, start + 1);
        if (end == std::string_view::npos) {
            return std::nullopt;
        }
        return std::string(tag.substr(start + 1, end - start - 1));
    }
    return std::nullopt;
}

// The CSS declarations in a style="..." attribute, as name -> value. The value
// keeps internal spaces (font-family:DejaVu Sans) but not surrounding ones.
std::map<std::string, std::string> parse_css(std::string_view style)
{
    std::map<std::string, std::string> out;
    std::size_t pos = 0;
    while (pos < style.size()) {
        const std::size_t semi = style.find(';', pos);
        const std::string_view decl = style.substr(
                pos, semi == std::string_view::npos ? std::string_view::npos : semi - pos);
        const std::size_t colon = decl.find(':');
        if (colon != std::string_view::npos) {
            const std::string key(trim(decl.substr(0, colon)));
            if (!key.empty()) {
                out[key] = std::string(trim(decl.substr(colon + 1)));
            }
        }
        if (semi == std::string_view::npos) {
            break;
        }
        pos = semi + 1;
    }
    return out;
}

// The leading number in a string, ignoring a trailing unit ("px"). nullopt
// when no number leads, so "medium" and "120px" are told apart.
std::optional<double> parse_number(std::string_view s)
{
    const char *begin = s.data();
    const char *end = s.data() + s.size();
    while (begin != end && std::isspace(static_cast<unsigned char>(*begin)) != 0) {
        ++begin;
    }
    double value = 0.0;
    const auto [ptr, ec] = std::from_chars(begin, end, value);
    if (ec != std::errc{ } || ptr == begin) {
        return std::nullopt;
    }
    return value;
}

// The raw styling read off one element, present only for the properties
// TextStyle can express.
struct RawStyle
{
    std::optional<std::string> font_family;
    std::optional<double> font_size_px;
    std::optional<std::string> font_weight;
    std::optional<std::string> font_style;
    std::optional<std::string> fill;
    std::optional<std::string> text_anchor;
};

// Reads an element's presentation attributes and style="..." declarations, the
// style attribute overriding the presentation attributes as SVG specifies.
RawStyle raw_style(std::string_view tag)
{
    RawStyle raw;
    if (const auto value = attribute_value(tag, "font-family")) {
        raw.font_family = *value;
    }
    if (const auto value = attribute_value(tag, "font-size")) {
        raw.font_size_px = parse_number(*value);
    }
    if (const auto value = attribute_value(tag, "font-weight")) {
        raw.font_weight = *value;
    }
    if (const auto value = attribute_value(tag, "font-style")) {
        raw.font_style = *value;
    }
    if (const auto value = attribute_value(tag, "fill")) {
        raw.fill = *value;
    }
    if (const auto value = attribute_value(tag, "text-anchor")) {
        raw.text_anchor = *value;
    }
    if (const auto style = attribute_value(tag, "style")) {
        const std::map<std::string, std::string> css = parse_css(*style);
        const auto take = [&css](const char *key) -> std::optional<std::string> {
            const auto it = css.find(key);
            return it == css.end() ? std::nullopt : std::optional<std::string>{ it->second };
        };
        if (const auto value = take("font-family")) {
            raw.font_family = *value;
        }
        if (const auto value = take("font-size")) {
            if (const auto px = parse_number(*value)) {
                raw.font_size_px = px;
            }
        }
        if (const auto value = take("font-weight")) {
            raw.font_weight = *value;
        }
        if (const auto value = take("font-style")) {
            raw.font_style = *value;
        }
        if (const auto value = take("fill")) {
            raw.fill = *value;
        }
        if (const auto value = take("text-anchor")) {
            raw.text_anchor = *value;
        }
    }
    return raw;
}

// True for a colour TextStyle can hold: "#rgb", "#rrggbb" or "#rrggbbaa".
bool is_hex_color(std::string_view s)
{
    if (s.size() < 2 || s.front() != '#') {
        return false;
    }
    const std::size_t len = s.size() - 1;
    if (len != 3 && len != 6 && len != 8) {
        return false;
    }
    return std::all_of(s.begin() + 1, s.end(),
                       [](char c) { return std::isxdigit(static_cast<unsigned char>(c)) != 0; });
}

// A family name with the surrounding quotes normalised to double quotes, as
// TextStyle's examples spell it ("Cabinet Grotesk"). Inkscape writes
// 'DejaVu Sans' with single quotes, so those are stripped first.
std::string quoted_family(std::string_view value)
{
    std::string_view inner = trim(value);
    if (inner.size() >= 2
        && ((inner.front() == '"' && inner.back() == '"')
            || (inner.front() == '\'' && inner.back() == '\''))) {
        inner = inner.substr(1, inner.size() - 2);
    }
    return "\"" + std::string(inner) + "\"";
}

// A CSS font-weight to the 100..=900 scale TextStyle holds. "bold" is what the
// templates use; the named steps and a bare number are accepted too.
double weight_from(std::string_view value)
{
    if (value == "bold" || value == "bolder") {
        return 700.0;
    }
    if (value == "normal") {
        return 400.0;
    }
    if (value == "lighter") {
        return 300.0;
    }
    if (const auto number = parse_number(value)) {
        return std::clamp(*number, 100.0, 900.0);
    }
    return 400.0;
}

// text-anchor to TextStyle's alignment. "middle" and anything unrecognised
// read as centred, matching the reader's own tolerance.
genesis::project::TextAlign align_from(std::string_view value)
{
    if (value == "start") {
        return genesis::project::TextAlign::Left;
    }
    if (value == "end") {
        return genesis::project::TextAlign::Right;
    }
    return genesis::project::TextAlign::Center;
}

// Applies one element's raw styling over `style`. Properties TextStyle cannot
// express (gradient fills, strokes, positions, line spacing) are simply not
// read - the README lists what is dropped.
void apply_raw(genesis::project::TextStyle &style, const RawStyle &raw)
{
    if (raw.font_family) {
        style.font_family = quoted_family(*raw.font_family);
    }
    if (raw.font_size_px) {
        style.font_size = *raw.font_size_px / REFERENCE_CANVAS_HEIGHT;
    }
    if (raw.font_weight) {
        style.font_weight = weight_from(*raw.font_weight);
    }
    if (raw.font_style) {
        style.italic = (*raw.font_style == "italic" || *raw.font_style == "oblique");
    }
    if (raw.fill && is_hex_color(*raw.fill)) {
        style.color = *raw.fill;
    }
    if (raw.text_anchor) {
        style.align = align_from(*raw.text_anchor);
    }
}

void apply_element(genesis::project::TextStyle &style, std::string_view tag)
{
    apply_raw(style, raw_style(tag));
}

// The concatenated text content of one <text> element's span, with every
// <tspan>'s styling applied over `style`. Tags are stripped, so a tspan and a
// bare text child yield the same words; a multi-tspan block is concatenated.
std::string span_text(std::string_view span, genesis::project::TextStyle &style)
{
    std::string text;
    std::size_t i = 0;
    while (i < span.size()) {
        const std::size_t open = span.find('<', i);
        if (open == std::string_view::npos) {
            text += decode_entities(span.substr(i));
            break;
        }
        text += decode_entities(span.substr(i, open - i));
        const std::size_t close = span.find('>', open);
        if (close == std::string_view::npos) {
            break; // Unterminated tag: the words already read stand.
        }
        const std::string_view tag = span.substr(open, close - open + 1);
        if (tag.rfind("<tspan", 0) == 0) {
            apply_element(style, tag);
        }
        i = close + 1;
    }
    return std::string(trim(text));
}

} // namespace

ParseResult parse_template(const std::filesystem::path &path)
{
    ParseResult result;
    std::ifstream in(path, std::ios::binary);
    if (!in) {
        result.reason = "cannot read file";
        return result;
    }
    std::ostringstream buffer;
    buffer << in.rdbuf();
    const std::string svg = buffer.str();

    std::vector<TitleSlot> slots;
    std::size_t pos = 0;
    while (pos < svg.size()) {
        const std::size_t open = svg.find("<text", pos);
        if (open == std::string::npos) {
            break;
        }
        // `<textPath` and the like are not title slots; require a word end.
        const std::size_t after_name = open + 5;
        if (after_name < svg.size() && is_name_char(svg[after_name])) {
            pos = after_name;
            continue;
        }
        const std::size_t open_end = svg.find('>', open);
        if (open_end == std::string::npos) {
            result.reason = "unterminated <text> tag";
            return result;
        }
        const std::size_t close = svg.find("</text>", open_end);
        if (close == std::string::npos) {
            result.reason = "unterminated <text> element";
            return result;
        }
        const std::string_view tag = std::string_view(svg).substr(open, open_end - open + 1);
        const std::string_view span =
                std::string_view(svg).substr(open_end + 1, close - open_end - 1);

        genesis::project::TextStyle style;
        apply_element(style, tag);
        std::string text = span_text(span, style);
        if (!text.empty()) {
            slots.push_back(TitleSlot{ slots.size(), std::move(text), style });
        }
        pos = close + 7;
    }

    if (slots.empty()) {
        result.reason = "no <text> elements";
        return result;
    }
    TitleTemplate tmpl;
    tmpl.name = path.stem().string();
    tmpl.path = path;
    tmpl.text_slots = std::move(slots);
    result.template_ = std::move(tmpl);
    return result;
}

TemplateLibrary discover_templates(const std::filesystem::path &directory)
{
    TemplateLibrary library;
    std::vector<std::filesystem::path> files;
    std::error_code ec;
    for (std::filesystem::directory_iterator it(directory, ec);
         !ec && it != std::filesystem::directory_iterator(); it.increment(ec)) {
        const std::filesystem::directory_entry entry = *it;
        std::error_code file_ec;
        if (!entry.is_regular_file(file_ec)) {
            continue;
        }
        if (entry.path().extension() != ".svg") {
            continue;
        }
        files.push_back(entry.path());
    }
    std::sort(files.begin(), files.end());
    for (const std::filesystem::path &path : files) {
        ParseResult parsed = parse_template(path);
        if (parsed.template_) {
            library.templates.push_back(std::move(*parsed.template_));
        } else {
            library.refusals.push_back(TemplateRefusal{ path, std::move(parsed.reason) });
        }
    }
    return library;
}

std::vector<std::pair<std::string, genesis::project::TextStyle>>
fill_slots(const TitleTemplate &tmpl, const std::vector<std::string> &texts)
{
    std::vector<std::pair<std::string, genesis::project::TextStyle>> out;
    out.reserve(tmpl.text_slots.size());
    for (const TitleSlot &slot : tmpl.text_slots) {
        const std::string &text = slot.index < texts.size() ? texts[slot.index] : slot.placeholder;
        out.emplace_back(text, slot.style);
    }
    return out;
}

} // namespace genesis::render
