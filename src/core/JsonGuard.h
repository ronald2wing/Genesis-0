// SPDX-License-Identifier: GPL-3.0-or-later
// SPDX-FileCopyrightText: 2026 Genesis-0 contributors

#pragma once

// Structural guards applied to a JSON *document* (a project, template, recents
// list or release manifest) before nlohmann's recursive parser runs. They are
// the document-scale counterpart to the JSON-RPC request guard in the CLI
// (cli::kMaxJsonRequestBytes / kMaxJsonDepth): a hostile or corrupted file is
// refused on its raw bytes, so it cannot drive the parser into unbounded
// recursion (a stack-exhaustion vector) or make it chew through an unbounded
// buffer. Header-only so the host and app targets both use it without a CMake
// change.

#include <istream>
#include <optional>
#include <string>
#include <string_view>

#include <nlohmann/json.hpp>

namespace genesis::core {

// The largest document the reader accepts: 64 MiB. Sixty-four times the 1 MiB
// JSON-RPC request cap, and far above any realistic project: a timeline with
// tens of thousands of clips and their keyframes serializes to a few MiB, so
// the bound is generous while still refusing a file sized to exhaust memory.
inline constexpr std::size_t kMaxDocumentJsonBytes = 64U << 20;

// The deepest nesting the reader accepts. Documents are shallow by construction
// (objects of arrays of objects); 512 levels is far beyond any real document
// while still short enough that the parser's recursion stays safe.
inline constexpr int kMaxDocumentJsonDepth = 512;

// Returns a human-readable reason `text` exceeds the document guards, or
// nullopt when it is within bounds. The scan tracks string literals and `\`
// escapes so a brace or bracket inside a string is never miscounted.
inline std::optional<std::string> document_json_guard(std::string_view text)
{
    if (text.size() > kMaxDocumentJsonBytes) {
        return "exceeds the 64 MiB document size limit";
    }
    int depth = 0;
    bool in_string = false;
    bool escaped = false;
    for (const char c : text) {
        if (in_string) {
            if (escaped) {
                escaped = false;
            } else if (c == '\\') {
                escaped = true;
            } else if (c == '"') {
                in_string = false;
            }
            continue;
        }
        if (c == '"') {
            in_string = true;
        } else if (c == '{' || c == '[') {
            if (++depth > kMaxDocumentJsonDepth) {
                return "nests deeper than 512 levels";
            }
        } else if (c == '}' || c == ']') {
            --depth;
        }
    }
    return std::nullopt;
}

// Parses a JSON document from `text`, applying the document guards first.
// Returns nullopt and fills `error` with a reason when the document is too
// large, nests too deep, or does not parse.
inline std::optional<nlohmann::json> parse_document_json(std::string_view text, std::string &error)
{
    if (const std::optional<std::string> problem = document_json_guard(text)) {
        error = *problem;
        return std::nullopt;
    }
    try {
        return nlohmann::json::parse(text);
    } catch (const nlohmann::json::parse_error &e) {
        error = e.what();
        return std::nullopt;
    }
}

// Parses a JSON document from `in`, reading at most kMaxDocumentJsonBytes
// before refusing, so an oversized file is rejected without first buffering it
// whole. Returns nullopt and fills `error` with a reason otherwise.
inline std::optional<nlohmann::json> parse_document_json(std::istream &in, std::string &error)
{
    std::string text;
    text.reserve(1U << 16);
    char buffer[65536];
    while (in) {
        in.read(buffer, sizeof(buffer));
        const std::streamsize got = in.gcount();
        if (got <= 0) {
            break;
        }
        if (text.size() + static_cast<std::size_t>(got) > kMaxDocumentJsonBytes) {
            error = "exceeds the 64 MiB document size limit";
            return std::nullopt;
        }
        text.append(buffer, static_cast<std::size_t>(got));
    }
    return parse_document_json(std::string_view(text), error);
}

} // namespace genesis::core
