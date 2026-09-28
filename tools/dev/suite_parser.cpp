// SPDX-License-Identifier: GPL-3.0-or-later
// SPDX-FileCopyrightText: 2026 Genesis-0 contributors

#include "tools/dev/suite_parser.h"

#include <cctype>

namespace genesis::dev {

namespace {

// Strips `#` comments to end of line so prose that mentions keywords never
// confuses the token scan.
std::string strip_comments(std::string_view text)
{
    std::string out;
    out.reserve(text.size());
    bool in_comment = false;
    for (const char c : text) {
        if (in_comment) {
            if (c == '\n') {
                in_comment = false;
                out += '\n';
            }
        } else if (c == '#') {
            in_comment = true;
        } else {
            out += c;
        }
    }
    return out;
}

bool is_word_char(char c)
{
    return std::isalnum(static_cast<unsigned char>(c)) != 0 || c == '_';
}

// Given `text` and the position of a `(` that opens a parenthesised group,
// returns the offset just past the matching `)` (or npos if unbalanced) and
// fills `inner` with the content between the parentheses. Parens inside are
// balanced, which is enough for both `if(...)` conditions and the
// genesis_add_test body (whose entries carry no parens).
std::size_t read_group(std::string_view text, std::size_t open_pos, std::string_view &inner)
{
    int depth = 0;
    for (std::size_t i = open_pos; i < text.size(); ++i) {
        if (text[i] == '(') {
            ++depth;
        } else if (text[i] == ')') {
            --depth;
            if (depth == 0) {
                inner = text.substr(open_pos + 1, i - open_pos - 1);
                return i + 1;
            }
        }
    }
    return std::string_view::npos;
}

// Splits `body` on whitespace and parses the genesis_add_test keyword grammar.
Suite parse_body(std::string_view body)
{
    Suite suite;
    enum class Field { None, Name, Sources, Libraries, WorkingDirectory } field = Field::None;

    std::size_t i = 0;
    const std::size_t n = body.size();
    while (i < n) {
        while (i < n && std::isspace(static_cast<unsigned char>(body[i])) != 0) {
            ++i;
        }
        const std::size_t start = i;
        while (i < n && std::isspace(static_cast<unsigned char>(body[i])) == 0) {
            ++i;
        }
        if (start == i) {
            break;
        }
        const std::string_view token = body.substr(start, i - start);

        if (token == "NAME") {
            field = Field::Name;
        } else if (token == "SOURCES") {
            field = Field::Sources;
        } else if (token == "LIBRARIES") {
            field = Field::Libraries;
        } else if (token == "WORKING_DIRECTORY") {
            field = Field::WorkingDirectory;
        } else if (token == "INCLUDE_DIRECTORIES" || token == "COMPILE_OPTIONS") {
            // Present in some registrations but irrelevant to test selection;
            // their values are consumed silently.
            field = Field::None;
        } else {
            switch (field) {
            case Field::Name:
                suite.name = std::string(token);
                field = Field::None;
                break;
            case Field::Sources:
                suite.sources.emplace_back(token);
                break;
            case Field::Libraries:
                suite.libraries.emplace_back(token);
                break;
            case Field::WorkingDirectory:
                suite.working_directory = std::string(token);
                field = Field::None;
                break;
            case Field::None:
                break;
            }
        }
    }
    return suite;
}

} // namespace

std::vector<Suite> parse_suites(std::string_view text)
{
    const std::string stripped = strip_comments(text);
    const std::string_view s = stripped;

    std::vector<Suite> suites;
    std::vector<std::string> gates;

    std::size_t i = 0;
    const std::size_t n = s.size();
    while (i < n) {
        if (std::isspace(static_cast<unsigned char>(s[i])) != 0) {
            ++i;
            continue;
        }
        const std::size_t start = i;
        while (i < n && is_word_char(s[i])) {
            ++i;
        }
        if (start == i) {
            // A non-word, non-whitespace character (parenthesis, `$`, `{`,
            // `:`, …). Skip it so the scan always advances.
            ++i;
            continue;
        }
        const std::string_view word = s.substr(start, i - start);

        if (word == "genesis_add_test" && i < n && s[i] == '(') {
            std::string_view body;
            i = read_group(s, i, body);
            if (i == std::string_view::npos) {
                break;
            }
            Suite suite = parse_body(body);
            if (!gates.empty()) {
                suite.gate = gates.back();
            }
            suites.push_back(std::move(suite));
            continue;
        }

        if (word == "if" && i < n && s[i] == '(') {
            std::string_view condition;
            i = read_group(s, i, condition);
            if (i == std::string_view::npos) {
                break;
            }
            gates.emplace_back(condition);
            continue;
        }
        if (word == "elseif" && i < n && s[i] == '(') {
            std::string_view condition;
            i = read_group(s, i, condition);
            if (i == std::string_view::npos) {
                break;
            }
            if (!gates.empty()) {
                gates.back() = std::string(condition);
            }
            continue;
        }
        if (word == "endif") {
            if (!gates.empty()) {
                gates.pop_back();
            }
            continue;
        }
    }

    return suites;
}

namespace {

// Splits `text` on whitespace into tokens.
std::vector<std::string> tokenize(std::string_view text)
{
    std::vector<std::string> tokens;
    std::size_t i = 0;
    const std::size_t n = text.size();
    while (i < n) {
        while (i < n && std::isspace(static_cast<unsigned char>(text[i])) != 0) {
            ++i;
        }
        const std::size_t start = i;
        while (i < n && std::isspace(static_cast<unsigned char>(text[i])) == 0) {
            ++i;
        }
        if (start < i) {
            tokens.emplace_back(text.substr(start, i - start));
        }
    }
    return tokens;
}

// Finds `marker` in `text` and returns the tokens of the parenthesised group
// that opens right after it, or an empty list when absent. Used for the
// library source lists: `set(GENESIS_HOST_SOURCES ...)` and
// `add_library(genesis_ai STATIC ...)`. The `(` need not be the marker's last
// character (e.g. `set(` opens before the variable name), so its position is
// searched, not assumed.
std::vector<std::string> tokens_of(std::string_view text, std::string_view marker)
{
    const std::size_t at = text.find(marker);
    if (at == std::string_view::npos) {
        return { };
    }
    const std::size_t open = text.find('(', at);
    if (open == std::string_view::npos) {
        return { };
    }
    std::string_view body;
    if (read_group(text, open, body) == std::string_view::npos) {
        return { };
    }
    return tokenize(body);
}

} // namespace

std::vector<std::pair<std::string, std::string>> library_sources(std::string_view text)
{
    const std::string stripped = strip_comments(text);
    std::vector<std::pair<std::string, std::string>> mapping;

    const auto add_library = [&](std::string_view marker, std::string_view library, int skip) {
        const std::vector<std::string> tokens = tokens_of(stripped, marker);
        for (std::size_t i = static_cast<std::size_t>(skip); i < tokens.size(); ++i) {
            mapping.emplace_back(std::string(library), tokens[i]);
        }
    };

    add_library("set(GENESIS_HOST_SOURCES", "genesis_host", 1); // skip the var name
    add_library("add_library(genesis_ai", "genesis_ai", 2); // skip name + STATIC
    add_library("add_library(genesis_engine_ges", "genesis_engine_ges", 2);

    return mapping;
}

} // namespace genesis::dev
