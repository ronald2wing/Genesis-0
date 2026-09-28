// SPDX-License-Identifier: GPL-3.0-or-later
// SPDX-FileCopyrightText: 2026 Genesis-0 contributors
//
// Shared TOML parsing primitives: id validation, source-location helpers,
// and typed field readers. All three parsers (Pack, ExtensionManifest, and the
// catalogue) use the same functions, so they live here rather than being
// duplicated across the tree.

#pragma once

#include <cstdint>
#include <optional>
#include <string>
#include <string_view>

#include <toml++/toml.hpp>

namespace genesis::core {

// --- id validation --------------------------------------------------------

// One segment of a namespaced id: lower-case letters, digits and hyphens.
bool is_id_segment(std::string_view text);

// `author.name`: two segments of `is_id_segment`, split on the first dot.
bool is_namespaced_id(std::string_view id);

// --- source-location helpers -----------------------------------------------

// "line L, column C", or empty where the position is unknown (a missing file).
std::string at(toml::source_position pos);

// A TOML path with the node's source location appended, e.g.
// `extension.menu[1].action (line 14, column 11)`.
std::string where_at(std::string_view path, const toml::node &node);

// The `where` for a parse failure: the source file when there is one, then the
// position toml++ reports.
std::string where_of(const toml::source_region &region);

// --- error sink ------------------------------------------------------------

// The error sink the shared readers report through. Each parser's Builder
// implements this by forwarding to its own problem list.
class TomlErrorSink
{
public:
    virtual ~TomlErrorSink() = default;
    virtual void fail(std::string where, std::string message) = 0;
};

// --- typed field readers --------------------------------------------------

void require_string(const toml::table &table, std::string_view key, std::string_view path,
                    std::string &out, TomlErrorSink &sink);

// Default-preserving string reader: absent key leaves `out`'s pre-set default.
void read_string(const toml::table &table, std::string_view key, std::string_view path,
                 std::string &out, TomlErrorSink &sink);

void read_optional_string(const toml::table &table, std::string_view key, std::string_view path,
                          std::optional<std::string> &out, TomlErrorSink &sink);

void read_uint32(const toml::table &table, std::string_view key, std::string_view path,
                 std::uint32_t &out, TomlErrorSink &sink);

void read_int32(const toml::table &table, std::string_view key, std::string_view path,
                std::int32_t &out, TomlErrorSink &sink);

void read_double(const toml::table &table, std::string_view key, std::string_view path, double &out,
                 TomlErrorSink &sink);

void read_bool(const toml::table &table, std::string_view key, std::string_view path, bool &out,
               TomlErrorSink &sink);

} // namespace genesis::core