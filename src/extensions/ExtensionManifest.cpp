// SPDX-License-Identifier: GPL-3.0-or-later
// SPDX-FileCopyrightText: 2026 Genesis-0 contributors

#include "extensions/ExtensionManifest.h"

#include <toml++/toml.hpp>

#include <cstdint>
#include <limits>
#include <optional>
#include <set>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

#include "core/TomlRead.h"
#include "effects/PackLoader.h"

namespace genesis::extensions {

namespace {

// A capability id: either a namespaced `author.name`, or the default form
// `<namespaced-id>:<digits>`. The colon splits the namespaced id from the
// per-kind index, which must be one or more digits.
bool is_capability_id(std::string_view id)
{
    const std::size_t colon = id.find(':');
    if (colon == std::string_view::npos) {
        return core::is_namespaced_id(id);
    }
    const std::string_view suffix = id.substr(colon + 1);
    if (suffix.empty()) {
        return false;
    }
    for (const char c : suffix) {
        if (c < '0' || c > '9') {
            return false;
        }
    }
    return core::is_namespaced_id(id.substr(0, colon));
}

// The override kind a manifest spells, or null for an unknown spelling. The
// kinds are `action` (a menu entry), `panel`, `dialog` and `effect` (an
// effect-pack id, e.g. `genesis.glow` or `genesis.packs:genesis.glow`).
std::optional<CapabilityKind> override_kind(std::string_view kind)
{
    if (kind == "action") {
        return CapabilityKind::Action;
    }
    if (kind == "panel") {
        return CapabilityKind::Panel;
    }
    if (kind == "dialog") {
        return CapabilityKind::Dialog;
    }
    if (kind == "effect") {
        return CapabilityKind::Effect;
    }
    return std::nullopt;
}

// Accumulates a manifest and the problems found while reading it. A problem
// recorded anywhere leaves the manifest unusable; the loader keeps reading so a
// human sees every problem at once rather than only the first.
struct Builder : core::TomlErrorSink
{
    ExtensionManifest manifest;
    std::vector<NativeLoadError> problems;

    void fail(std::string where, std::string what) override
    {
        problems.push_back(NativeLoadError{ std::move(where), std::move(what) });
    }
};

// A slot `priority`: a whole number in 0..100, defaulting to 50 when absent.
void read_priority(const toml::table &table, std::string_view key, std::string_view path,
                   std::int32_t &out, Builder &builder)
{
    const toml::node *node = table.get(key);
    if (node == nullptr) {
        return; // the schema's 50 stands
    }
    const auto value = node->value_exact<std::int64_t>();
    if (!value || *value < 0 || *value > 100) {
        builder.fail(core::where_at(path, *node),
                     "`" + std::string(key) + "` must be a whole number between 0 and 100");
        return;
    }
    out = static_cast<std::int32_t>(*value);
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
        builder.fail(core::where_at("extension", *extension_node), "[extension] must be a table");
        return;
    }

    core::require_string(*extension, "id", "extension.id", builder.manifest.id, builder);
    core::require_string(*extension, "name", "extension.name", builder.manifest.name, builder);
    core::read_optional_string(*extension, "entry", "extension.entry", builder.manifest.entry,
                               builder);

    core::read_uint32(*extension, "version", "extension.version", builder.manifest.version,
                      builder);
    core::read_uint32(*extension, "api", "extension.api", builder.manifest.api, builder);

    const toml::node *packs_node = extension->get("packs");
    if (packs_node != nullptr) {
        const toml::array *packs = packs_node->as_array();
        if (packs == nullptr) {
            builder.fail(core::where_at("extension.packs", *packs_node),
                         "`packs` must be an array");
        } else {
            std::size_t i = 0;
            for (const toml::node &pack : *packs) {
                const auto id = pack.value<std::string>();
                if (!id) {
                    builder.fail(core::where_at("extension.packs[" + std::to_string(i) + "]", pack),
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
            builder.fail(core::where_at("extension.requires", *requires_node),
                         "`requires` must be an array");
        } else {
            std::size_t i = 0;
            for (const toml::node &required : *required_array) {
                const auto id = required.value<std::string>();
                if (!id) {
                    builder.fail(core::where_at("extension.requires[" + std::to_string(i) + "]",
                                                required),
                                 "must be a string");
                } else {
                    builder.manifest.dependencies.push_back(*id);
                }
                ++i;
            }
        }
    }

    // The `overrides` declarations: `<kind>:<id>` strings. The kind is one of
    // `action`/`panel`/`dialog`/`effect`, split on the first colon; the id is
    // the other extension's capability id (namespaced or the default
    // `<ext-id>:<index>` form, which itself carries a colon). `effect` names an
    // effect-pack id (e.g. `genesis.glow` or `genesis.packs:genesis.glow`).
    // Whether this extension is trusted to override at all is a separate gate
    // (see Trust.h's `may_override`), applied at install and scan — not here,
    // where the manifest has no origin.
    const toml::node *overrides_node = extension->get("overrides");
    if (overrides_node != nullptr) {
        const toml::array *overrides = overrides_node->as_array();
        if (overrides == nullptr) {
            builder.fail(core::where_at("extension.overrides", *overrides_node),
                         "`overrides` must be an array");
        } else {
            std::size_t i = 0;
            for (const toml::node &entry : *overrides) {
                const std::string path = "extension.overrides[" + std::to_string(i) + "]";
                const auto text = entry.value<std::string>();
                if (!text) {
                    builder.fail(core::where_at(path, entry), "must be a string");
                    ++i;
                    continue;
                }
                const std::size_t colon = text->find(':');
                const std::string_view kind = colon == std::string::npos
                        ? std::string_view(*text)
                        : std::string_view(*text).substr(0, colon);
                const std::string id =
                        colon == std::string::npos ? std::string() : text->substr(colon + 1);
                const std::optional<CapabilityKind> parsed_kind = override_kind(kind);
                if (!parsed_kind) {
                    builder.fail(core::where_at(path, entry),
                                 "`" + *text
                                         + "` must be `<kind>:<id>` with kind `action`, `panel`"
                                           ", `dialog` or `effect`");
                } else if (id.empty()) {
                    builder.fail(core::where_at(path, entry),
                                 "`" + *text + "` must be `<kind>:<id>` with a non-empty id");
                } else {
                    builder.manifest.overrides.push_back(OverrideEntry{ *parsed_kind, id });
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
        builder.fail(core::where_at("menu", *menus_node), "`menu` must be an array of tables");
        return;
    }
    std::size_t index = 0;
    for (const toml::node &element : *menus) {
        const std::string path = "menu[" + std::to_string(index) + "]";
        const toml::table *table = element.as_table();
        if (table == nullptr) {
            builder.fail(core::where_at(path, element), "must be a table");
            ++index;
            continue;
        }

        MenuEntry menu;
        // The default capability id: `<ext-id>:<per-kind index>`.
        menu.id = builder.manifest.id + ":" + std::to_string(index);
        core::read_string(*table, "id", path + ".id", menu.id, builder);
        core::require_string(*table, "path", path + ".path", menu.path, builder);
        core::require_string(*table, "label", path + ".label", menu.label, builder);
        core::read_optional_string(*table, "action", path + ".action", menu.action, builder);
        core::read_optional_string(*table, "call", path + ".call", menu.call, builder);
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
        builder.fail(core::where_at("panel", *panels_node), "`panel` must be an array of tables");
        return;
    }
    std::size_t index = 0;
    for (const toml::node &element : *panels) {
        const std::string path = "panel[" + std::to_string(index) + "]";
        const toml::table *table = element.as_table();
        if (table == nullptr) {
            builder.fail(core::where_at(path, element), "must be a table");
            ++index;
            continue;
        }

        Panel panel;
        panel.id = builder.manifest.id + ":" + std::to_string(index);
        core::read_string(*table, "id", path + ".id", panel.id, builder);
        core::require_string(*table, "label", path + ".label", panel.label, builder);
        core::require_string(*table, "qml", path + ".qml", panel.qml, builder);
        builder.manifest.panels.push_back(std::move(panel));
        ++index;
    }
}

void read_dialogs(const toml::table &root, Builder &builder)
{
    const toml::node *dialogs_node = root.get("dialog");
    if (dialogs_node == nullptr) {
        return;
    }
    const toml::array *dialogs = dialogs_node->as_array();
    if (dialogs == nullptr) {
        builder.fail(core::where_at("dialog", *dialogs_node),
                     "`dialog` must be an array of tables");
        return;
    }
    std::size_t index = 0;
    for (const toml::node &element : *dialogs) {
        const std::string path = "dialog[" + std::to_string(index) + "]";
        const toml::table *table = element.as_table();
        if (table == nullptr) {
            builder.fail(core::where_at(path, element), "must be a table");
            ++index;
            continue;
        }

        Dialog dialog;
        dialog.id = builder.manifest.id + ":" + std::to_string(index);
        core::read_string(*table, "id", path + ".id", dialog.id, builder);
        core::require_string(*table, "label", path + ".label", dialog.label, builder);
        core::require_string(*table, "qml", path + ".qml", dialog.qml, builder);
        core::read_optional_string(*table, "menu_path", path + ".menu_path", dialog.menu_path,
                                   builder);
        builder.manifest.dialogs.push_back(std::move(dialog));
        ++index;
    }
}

void read_slots(const toml::table &root, Builder &builder)
{
    const toml::node *slots_node = root.get("slot");
    if (slots_node == nullptr) {
        return;
    }
    const toml::array *slots = slots_node->as_array();
    if (slots == nullptr) {
        builder.fail(core::where_at("slot", *slots_node), "`slot` must be an array of tables");
        return;
    }
    std::size_t index = 0;
    for (const toml::node &element : *slots) {
        const std::string path = "slot[" + std::to_string(index) + "]";
        const toml::table *table = element.as_table();
        if (table == nullptr) {
            builder.fail(core::where_at(path, element), "must be a table");
            ++index;
            continue;
        }

        Slot slot;
        core::require_string(*table, "point", path + ".point", slot.point, builder);
        core::require_string(*table, "qml", path + ".qml", slot.qml, builder);
        read_priority(*table, "priority", path + ".priority", slot.priority, builder);
        builder.manifest.slot_entries.push_back(std::move(slot));
        ++index;
    }
}

// Reads `[[effect]]` blocks from the top-level `effect` array, parsing each
// into a Pack through the reusable `load_effect_table`. The sub-tables (param,
// uniform, wgsl, etc.) are scoped inside each effect block, not at top level.
void read_effects(const toml::table &root, Builder &builder)
{
    const toml::node *effects_node = root.get("effect");
    if (effects_node == nullptr) {
        return;
    }
    const toml::array *effects = effects_node->as_array();
    if (effects == nullptr) {
        builder.fail(core::where_at("effect", *effects_node),
                     "`effect` must be an array of tables (use `[[effect]]`)");
        return;
    }
    // `[[effect]]` blocks contribute the extension's effect packs, parsed here in
    // manifest order into `effects`. There is no package-level `kind` field —
    // any extension (with or without an `entry`) may ship effects; the presence
    // of `entry` alone marks it as native.
    std::size_t index = 0;
    for (const toml::node &element : *effects) {
        const std::string path = "effect[" + std::to_string(index) + "]";
        const toml::table *table = element.as_table();
        if (table == nullptr) {
            builder.fail(core::where_at(path, element), "must be a table");
            ++index;
            continue;
        }

        const genesis::effects::LoadResult loaded =
                genesis::effects::load_effect_table(*table, path);
        if (!loaded.problems.empty()) {
            for (const genesis::effects::LoadError &problem : loaded.problems) {
                builder.fail(problem.where, problem.what);
            }
        } else if (loaded.pack) {
            builder.manifest.effects.push_back(std::move(*loaded.pack));
        }
        ++index;
    }
}

// Reads every table the schema carries. There is exactly one manifest format,
// so no version gate is read; an unknown top-level key (for example an obsolete
// `format` left over from an older manifest) is ignored, the reader's normal
// look-up-only policy.
void fill_manifest(const toml::table &root, Builder &builder)
{
    read_extension(root, builder);
    read_effects(root, builder);
    read_menus(root, builder);
    read_panels(root, builder);
    read_dialogs(root, builder);
    read_slots(root, builder);
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
        builder.fail(core::where_of(error.source()), std::string(error.description()));
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
        builder.fail(core::where_of(error.source()), std::string(error.description()));
    } catch (const std::exception &error) {
        builder.fail(path.string(), error.what());
    }
    return finish(std::move(builder));
}

// --- validation ---------------------------------------------------------

namespace {

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

// The documented host surfaces a slot may embed into. An unknown point is a
// manifest refusal, not a silent drop: the host only renders the points it
// knows, so a mistyped point name must fail loudly rather than vanish a
// contribution the author expected to see.
bool is_known_slot_point(std::string_view point)
{
    return point == "clip.inspector" || point == "status.bar" || point == "settings.updates";
}

} // namespace

std::vector<std::string> validate_native(const ExtensionManifest &manifest)
{
    std::vector<std::string> problems;
    const auto invalid = [&](std::string message) {
        problems.push_back(manifest.id + ": " + std::move(message));
    };

    if (!core::is_namespaced_id(manifest.id)) {
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
    std::set<std::string> menu_ids;
    for (const MenuEntry &menu : manifest.menus) {
        if (!is_capability_id(menu.id)) {
            invalid("menu id `" + menu.id + "` must be namespaced or `<ext-id>:<index>`");
        }
        if (!menu_ids.insert(menu.id).second) {
            invalid("duplicate menu id `" + menu.id + "`");
        }
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
    std::set<std::string> panel_ids;
    for (const Panel &panel : manifest.panels) {
        if (!is_capability_id(panel.id)) {
            invalid("panel id `" + panel.id + "` must be namespaced or `<ext-id>:<index>`");
        }
        if (!panel_ids.insert(panel.id).second) {
            invalid("duplicate panel id `" + panel.id + "`");
        }
        if (is_blank(panel.label)) {
            invalid("a panel has no label");
        }
        if (!is_safe_entry(panel.qml)) {
            invalid("panel qml `" + panel.qml + "` must be a file name beside the manifest");
        }
    }
    std::set<std::string> dialog_ids;
    for (const Dialog &dialog : manifest.dialogs) {
        if (!is_capability_id(dialog.id)) {
            invalid("dialog id `" + dialog.id + "` must be namespaced or `<ext-id>:<index>`");
        }
        if (!dialog_ids.insert(dialog.id).second) {
            invalid("duplicate dialog id `" + dialog.id + "`");
        }
        if (is_blank(dialog.label)) {
            invalid("a dialog has no label");
        }
        if (!is_safe_entry(dialog.qml)) {
            invalid("dialog qml `" + dialog.qml + "` must be a file name beside the manifest");
        }
        if (dialog.menu_path && is_blank(*dialog.menu_path)) {
            invalid("dialog `" + dialog.qml + "` has a blank menu_path");
        }
    }
    for (const Slot &slot : manifest.slot_entries) {
        if (!is_known_slot_point(slot.point)) {
            invalid("slot point `" + slot.point + "` is not a known host surface");
        }
        if (!is_safe_entry(slot.qml)) {
            invalid("slot qml `" + slot.qml + "` must be a file name beside the manifest");
        }
    }
    for (const std::string &pack : manifest.packs) {
        if (!core::is_namespaced_id(pack)) {
            invalid("bundled pack `" + pack + "` is not a namespaced id");
        }
    }
    for (const std::string &required : manifest.dependencies) {
        if (!core::is_namespaced_id(required)) {
            invalid("required extension `" + required + "` is not a namespaced id");
        }
    }
    for (const OverrideEntry &override : manifest.overrides) {
        if (!is_capability_id(override.id)) {
            invalid("override id `" + override.id + "` must be namespaced or `<ext-id>:<index>`");
        }
    }
    if (!manifest.entry && has_action_menu) {
        invalid("entry is required: a menu `action` runs the extension's library");
    }
    return problems;
}

} // namespace genesis::extensions
