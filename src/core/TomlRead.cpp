// SPDX-License-Identifier: GPL-3.0-or-later
// SPDX-FileCopyrightText: 2026 Genesis-0 contributors

#include "core/TomlRead.h"

#include <limits>
#include <string>
#include <string_view>

#include <toml++/toml.hpp>

namespace genesis::core {

// --- id validation --------------------------------------------------------

bool is_id_segment(std::string_view text)
{
    if (text.empty()) {
        return false;
    }
    for (const char c : text) {
        if (!((c >= 'a' && c <= 'z') || (c >= '0' && c <= '9') || c == '-')) {
            return false;
        }
    }
    return true;
}

bool is_namespaced_id(std::string_view id)
{
    const std::size_t dot = id.find('.');
    if (dot == std::string_view::npos) {
        return false;
    }
    return is_id_segment(id.substr(0, dot)) && is_id_segment(id.substr(dot + 1));
}

// --- source-location helpers -----------------------------------------------

std::string at(toml::source_position pos)
{
    if (!pos) {
        return { };
    }
    return "line " + std::to_string(pos.line) + ", column " + std::to_string(pos.column);
}

std::string where_at(std::string_view path, const toml::node &node)
{
    const std::string location = at(node.source().begin);
    if (location.empty()) {
        return std::string(path);
    }
    std::string out(path);
    out += " (";
    out += location;
    out += ")";
    return out;
}

std::string where_of(const toml::source_region &region)
{
    std::string out;
    if (region.path && !region.path->empty()) {
        out = *region.path;
    }
    const std::string location = at(region.begin);
    if (!location.empty()) {
        if (!out.empty()) {
            out += ": ";
        }
        out += location;
    }
    if (out.empty()) {
        out = "<manifest>";
    }
    return out;
}

// --- typed field readers --------------------------------------------------

void require_string(const toml::table &table, std::string_view key, std::string_view path,
                    std::string &out, TomlErrorSink &sink)
{
    const toml::node *node = table.get(key);
    if (node == nullptr) {
        sink.fail(std::string(path), "missing `" + std::string(key) + "`");
        return;
    }
    const auto value = node->value<std::string>();
    if (!value) {
        sink.fail(where_at(path, *node), "`" + std::string(key) + "` must be a string");
        return;
    }
    out = *value;
}

void read_string(const toml::table &table, std::string_view key, std::string_view path,
                 std::string &out, TomlErrorSink &sink)
{
    const toml::node *node = table.get(key);
    if (node == nullptr) {
        return;
    }
    const auto value = node->value<std::string>();
    if (!value) {
        sink.fail(where_at(path, *node), "`" + std::string(key) + "` must be a string");
        return;
    }
    out = *value;
}

void read_optional_string(const toml::table &table, std::string_view key, std::string_view path,
                          std::optional<std::string> &out, TomlErrorSink &sink)
{
    const toml::node *node = table.get(key);
    if (node == nullptr) {
        return;
    }
    const auto value = node->value<std::string>();
    if (!value) {
        sink.fail(where_at(path, *node), "`" + std::string(key) + "` must be a string");
        return;
    }
    out = *value;
}

void read_uint32(const toml::table &table, std::string_view key, std::string_view path,
                 std::uint32_t &out, TomlErrorSink &sink)
{
    const toml::node *node = table.get(key);
    if (node == nullptr) {
        return;
    }
    const auto value = node->value_exact<std::int64_t>();
    if (!value || *value < 0
        || static_cast<std::uint64_t>(*value)
                > static_cast<std::uint64_t>(std::numeric_limits<std::uint32_t>::max())) {
        sink.fail(where_at(path, *node), "`" + std::string(key) + "` must be a whole number");
        return;
    }
    out = static_cast<std::uint32_t>(*value);
}

void read_int32(const toml::table &table, std::string_view key, std::string_view path,
                std::int32_t &out, TomlErrorSink &sink)
{
    const toml::node *node = table.get(key);
    if (node == nullptr) {
        return;
    }
    const auto value = node->value_exact<std::int64_t>();
    if (!value || *value < std::numeric_limits<std::int32_t>::min()
        || *value > std::numeric_limits<std::int32_t>::max()) {
        sink.fail(where_at(path, *node), "`" + std::string(key) + "` must be a whole number");
        return;
    }
    out = static_cast<std::int32_t>(*value);
}

void read_double(const toml::table &table, std::string_view key, std::string_view path, double &out,
                 TomlErrorSink &sink)
{
    const toml::node *node = table.get(key);
    if (node == nullptr) {
        return;
    }
    const auto value = node->value<double>();
    if (!value) {
        sink.fail(where_at(path, *node), "`" + std::string(key) + "` must be a number");
        return;
    }
    out = *value;
}

void read_bool(const toml::table &table, std::string_view key, std::string_view path, bool &out,
               TomlErrorSink &sink)
{
    const toml::node *node = table.get(key);
    if (node == nullptr) {
        return;
    }
    const auto value = node->value_exact<bool>();
    if (!value) {
        sink.fail(where_at(path, *node), "`" + std::string(key) + "` must be true or false");
        return;
    }
    out = *value;
}

} // namespace genesis::core