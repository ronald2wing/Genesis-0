// SPDX-License-Identifier: GPL-3.0-or-later
// SPDX-FileCopyrightText: 2026 Genesis-0 contributors

#include "extensions/NativeManifest.h"

#include <toml++/toml.hpp>

#include <cstdint>
#include <limits>
#include <optional>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

namespace genesis::extensions {

namespace {

// The manifest format this build reads: the shape of a native extension. Absent
// is treated as 1 (the historical default). Formats 1 and 2 both read (format 1
// is the pre-dependency shape, so its `requires` is empty); anything above 2 is
// a newer build's manifest and is rejected, never half-loaded.
inline constexpr std::uint32_t FORMAT = 2;

// "line L, column C", or empty where the position is unknown (a missing file).
std::string at(toml::source_position pos)
{
    if (!pos) {
        return { };
    }
    return "line " + std::to_string(pos.line) + ", column " + std::to_string(pos.column);
}

// A TOML path with the node's source location appended, e.g.
// `extension.menu[1].action (line 14, column 11)`.
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

// The `where` for a parse failure: the source file when there is one, then the
// position toml++ reports.
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

// Accumulates a manifest and the problems found while reading it. A problem
// recorded anywhere leaves the manifest unusable; the loader keeps reading so a
// human sees every problem at once rather than only the first.
struct Builder
{
    NativeManifest manifest;
    std::vector<NativeLoadError> problems;

    void fail(std::string where, std::string what)
    {
        problems.push_back(NativeLoadError{ std::move(where), std::move(what) });
    }
};

// --- typed field readers ------------------------------------------------
//
// Each reads one key of a table into a schema field, records a problem on a
// wrong type, and leaves the field untouched when the key is absent (so the
// schema's own defaults stand). `require_string` is the required variant.

void require_string(const toml::table &table, std::string_view key, std::string_view path,
                    std::string &out, Builder &builder)
{
    const toml::node *node = table.get(key);
    if (node == nullptr) {
        builder.fail(std::string(path), "missing `" + std::string(key) + "`");
        return;
    }
    const auto value = node->value<std::string>();
    if (!value) {
        builder.fail(where_at(path, *node), "`" + std::string(key) + "` must be a string");
        return;
    }
    out = *value;
}

void read_optional_string(const toml::table &table, std::string_view key, std::string_view path,
                          std::optional<std::string> &out, Builder &builder)
{
    const toml::node *node = table.get(key);
    if (node == nullptr) {
        return;
    }
    const auto value = node->value<std::string>();
    if (!value) {
        builder.fail(where_at(path, *node), "`" + std::string(key) + "` must be a string");
        return;
    }
    out = *value;
}

void read_uint32(const toml::table &table, std::string_view key, std::string_view path,
                 std::uint32_t &out, Builder &builder)
{
    const toml::node *node = table.get(key);
    if (node == nullptr) {
        return;
    }
    const auto value = node->value_exact<std::int64_t>();
    if (!value || *value < 0
        || static_cast<std::uint64_t>(*value)
                > static_cast<std::uint64_t>(std::numeric_limits<std::uint32_t>::max())) {
        builder.fail(where_at(path, *node), "`" + std::string(key) + "` must be a whole number");
        return;
    }
    out = static_cast<std::uint32_t>(*value);
}

// --- table readers ------------------------------------------------------

void read_extension(const toml::table &root, Builder &builder)
{
    const toml::node *extension_node = root.get("extension");
    if (extension_node == nullptr) {
        builder.fail("extension", "missing [extension] table");
        return;
    }
    const toml::table *extension = extension_node->as_table();
    if (extension == nullptr) {
        builder.fail(where_at("extension", *extension_node), "[extension] must be a table");
        return;
    }

    require_string(*extension, "id", "extension.id", builder.manifest.id, builder);
    require_string(*extension, "name", "extension.name", builder.manifest.name, builder);
    read_optional_string(*extension, "entry", "extension.entry", builder.manifest.entry, builder);

    const toml::node *kind_node = extension->get("kind");
    if (kind_node == nullptr) {
        builder.fail("extension.kind", "missing `kind`");
    } else {
        const auto kind = kind_node->value<std::string>();
        if (!kind) {
            builder.fail(where_at("extension.kind", *kind_node), "`kind` must be a string");
        } else if (*kind != "native") {
            builder.fail(where_at("extension.kind", *kind_node),
                         "`kind` must be `native` (this manifest is a native "
                         "extension, not a Pack)");
        }
    }

    read_uint32(*extension, "version", "extension.version", builder.manifest.version, builder);
    read_uint32(*extension, "api", "extension.api", builder.manifest.api, builder);

    const toml::node *packs_node = extension->get("packs");
    if (packs_node != nullptr) {
        const toml::array *packs = packs_node->as_array();
        if (packs == nullptr) {
            builder.fail(where_at("extension.packs", *packs_node), "`packs` must be an array");
        } else {
            std::size_t i = 0;
            for (const toml::node &pack : *packs) {
                const auto id = pack.value<std::string>();
                if (!id) {
                    builder.fail(where_at("extension.packs[" + std::to_string(i) + "]", pack),
                                 "must be a string");
                } else {
                    builder.manifest.packs.push_back(*id);
                }
                ++i;
            }
        }
    }

    const toml::node *requires_node = extension->get("requires");
    if (requires_node != nullptr) {
        const toml::array *required_array = requires_node->as_array();
        if (required_array == nullptr) {
            builder.fail(where_at("extension.requires", *requires_node),
                         "`requires` must be an array");
        } else {
            std::size_t i = 0;
            for (const toml::node &required : *required_array) {
                const auto id = required.value<std::string>();
                if (!id) {
                    builder.fail(
                            where_at("extension.requires[" + std::to_string(i) + "]", required),
                            "must be a string");
                } else {
                    builder.manifest.dependencies.push_back(*id);
                }
                ++i;
            }
        }
    }
}

void read_menus(const toml::table &root, Builder &builder)
{
    const toml::node *menus_node = root.get("menu");
    if (menus_node == nullptr) {
        return;
    }
    const toml::array *menus = menus_node->as_array();
    if (menus == nullptr) {
        builder.fail(where_at("menu", *menus_node), "`menu` must be an array of tables");
        return;
    }
    std::size_t index = 0;
    for (const toml::node &element : *menus) {
        const std::string path = "menu[" + std::to_string(index) + "]";
        const toml::table *table = element.as_table();
        if (table == nullptr) {
            builder.fail(where_at(path, element), "must be a table");
            ++index;
            continue;
        }

        MenuEntry menu;
        require_string(*table, "path", path + ".path", menu.path, builder);
        require_string(*table, "label", path + ".label", menu.label, builder);
        read_optional_string(*table, "action", path + ".action", menu.action, builder);
        read_optional_string(*table, "call", path + ".call", menu.call, builder);
        builder.manifest.menus.push_back(std::move(menu));
        ++index;
    }
}

void read_panels(const toml::table &root, Builder &builder)
{
    const toml::node *panels_node = root.get("panel");
    if (panels_node == nullptr) {
        return;
    }
    const toml::array *panels = panels_node->as_array();
    if (panels == nullptr) {
        builder.fail(where_at("panel", *panels_node), "`panel` must be an array of tables");
        return;
    }
    std::size_t index = 0;
    for (const toml::node &element : *panels) {
        const std::string path = "panel[" + std::to_string(index) + "]";
        const toml::table *table = element.as_table();
        if (table == nullptr) {
            builder.fail(where_at(path, element), "must be a table");
            ++index;
            continue;
        }

        Panel panel;
        require_string(*table, "label", path + ".label", panel.label, builder);
        require_string(*table, "qml", path + ".qml", panel.qml, builder);
        builder.manifest.panels.push_back(std::move(panel));
        ++index;
    }
}

// Reads the format gate, then every table the schema carries. A rejected format
// stops here; the manifest is not half-built.
void fill_manifest(const toml::table &root, Builder &builder)
{
    std::uint32_t format = FORMAT;
    const toml::node *format_node = root.get("format");
    if (format_node != nullptr) {
        const auto value = format_node->value_exact<std::int64_t>();
        if (!value || *value < 0
            || static_cast<std::uint64_t>(*value)
                    > static_cast<std::uint64_t>(std::numeric_limits<std::uint32_t>::max())) {
            builder.fail(where_at("format", *format_node), "`format` must be a whole number");
            return;
        }
        format = static_cast<std::uint32_t>(*value);
    }
    if (format == 0) {
        builder.fail("format", "format is 0; the first extension format is 1");
        return;
    }
    if (format > FORMAT) {
        builder.fail("format",
                     "written for extension format " + std::to_string(format)
                             + ", and this build reads up to " + std::to_string(FORMAT));
        return;
    }

    read_extension(root, builder);
    read_menus(root, builder);
    read_panels(root, builder);
}

// Folds the schema's own validation into the problems (rather than re-reading
// its rules here), then decides whether the manifest survives: any problem
// leaves `manifest` empty.
NativeLoadResult finish(Builder builder)
{
    if (builder.problems.empty()) {
        for (const std::string &message : validate_native(builder.manifest)) {
            NativeLoadError problem;
            problem.where = builder.manifest.id;
            problem.what = message;
            builder.problems.push_back(std::move(problem));
        }
    }
    NativeLoadResult result;
    if (builder.problems.empty()) {
        result.manifest = std::move(builder.manifest);
    }
    result.problems = std::move(builder.problems);
    return result;
}

} // namespace

NativeLoadResult load_native_manifest(std::string_view toml_text)
{
    Builder builder;
    try {
        const toml::table root = toml::parse(toml_text);
        fill_manifest(root, builder);
    } catch (const toml::parse_error &error) {
        builder.fail(where_of(error.source()), std::string(error.description()));
    } catch (const std::exception &error) {
        builder.fail("<manifest>", error.what());
    }
    return finish(std::move(builder));
}

NativeLoadResult load_native_manifest_file(const std::filesystem::path &path)
{
    Builder builder;
    try {
        const std::string path_text = path.string();
        const toml::table root = toml::parse_file(path_text);
        fill_manifest(root, builder);
    } catch (const toml::parse_error &error) {
        builder.fail(where_of(error.source()), std::string(error.description()));
    } catch (const std::exception &error) {
        builder.fail(path.string(), error.what());
    }
    return finish(std::move(builder));
}

// --- validation ---------------------------------------------------------

namespace {

// One segment of a namespaced id: lower-case letters, digits and hyphens.
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

// `author.name`: two segments of `is_id_segment`, split on the first dot.
bool is_namespaced_id(std::string_view id)
{
    const std::size_t dot = id.find('.');
    if (dot == std::string_view::npos) {
        return false;
    }
    return is_id_segment(id.substr(0, dot)) && is_id_segment(id.substr(dot + 1));
}

// Empty but for ASCII whitespace, matching the manifest's `.trim()`.
bool is_blank(std::string_view text)
{
    for (const char c : text) {
        if (c != ' ' && c != '\t' && c != '\n' && c != '\r') {
            return false;
        }
    }
    return true;
}

// A shared-library filename: a safe path segment (no separator, no `..`) that
// names a file. `entry` is "beside the manifest", so it must not escape the
// extension's directory.
bool is_safe_entry(std::string_view text)
{
    return !text.empty() && text.find('/') == std::string_view::npos
            && text.find('\\') == std::string_view::npos
            && text.find("..") == std::string_view::npos;
}

} // namespace

std::vector<std::string> validate_native(const NativeManifest &manifest)
{
    std::vector<std::string> problems;
    const auto invalid = [&](std::string message) {
        problems.push_back(manifest.id + ": " + std::move(message));
    };

    if (!is_namespaced_id(manifest.id)) {
        invalid("id must be `author.name`, lower-case letters, digits and hyphens");
    }
    if (is_blank(manifest.name)) {
        invalid("name is empty");
    }
    if (manifest.entry && !is_safe_entry(*manifest.entry)) {
        invalid("entry must be a file name beside the manifest");
    }
    if (manifest.api == 0) {
        invalid("api must name an extension API version, not 0");
    }
    bool has_action_menu = false;
    for (const MenuEntry &menu : manifest.menus) {
        if (is_blank(menu.path)) {
            invalid("a menu entry has no path");
        }
        if (is_blank(menu.label)) {
            invalid("menu `" + menu.path + "` has no label");
        }
        const bool has_action = menu.action && !menu.action->empty();
        const bool has_call = menu.call && !menu.call->empty();
        if (has_action == has_call) {
            invalid("menu `" + menu.path + "` must set exactly one of `action` or `call`");
        }
        has_action_menu = has_action_menu || has_action;
    }
    for (const Panel &panel : manifest.panels) {
        if (is_blank(panel.label)) {
            invalid("a panel has no label");
        }
        if (!is_safe_entry(panel.qml)) {
            invalid("panel qml `" + panel.qml + "` must be a file name beside the manifest");
        }
    }
    for (const std::string &pack : manifest.packs) {
        if (!is_namespaced_id(pack)) {
            invalid("bundled pack `" + pack + "` is not a namespaced id");
        }
    }
    for (const std::string &required : manifest.dependencies) {
        if (!is_namespaced_id(required)) {
            invalid("required extension `" + required + "` is not a namespaced id");
        }
    }
    if (!manifest.entry && has_action_menu) {
        invalid("entry is required: a menu `action` runs the extension's library");
    }
    return problems;
}

} // namespace genesis::extensions
