// SPDX-License-Identifier: GPL-3.0-or-later
// SPDX-FileCopyrightText: 2026 Genesis-0 contributors

#pragma once

#include <cstdint>
#include <filesystem>
#include <optional>
#include <string>
#include <string_view>
#include <vector>

#include "effects/Pack.h"
#include "extensions/Trust.h"

namespace genesis::extensions {

// The id forms a capability (a menu `action`, a panel, or a dialog) may carry.
// Either a namespaced `author.name`, or the default form `<ext-id>:<index>`
// (the extension's id, a colon, then the capability's 0-based per-kind index).
// The host keys override resolution on this id, so it must be stable across
// rescans and unique within its kind per extension.
//
// One menu entry a native extension contributes: a command the host surfaces
// that either invokes the extension's `invoke(action)` or routes a declarative
// `call` to the host API through the injected bridge. Exactly one of `action`
// and `call` is set.
struct MenuEntry
{
    std::string id; // capability id, default `<ext-id>:<index>` (see above)
    std::string path; // "Tools/Smart Blur"
    std::string label; // "Smart Blur"
    std::optional<std::string> action;
    std::optional<std::string> call;
};

// A QML panel the extension contributes. `qml` names a file beside the manifest
// the host loads into the extension panel area. Arbitrary QML is the
// extension's own trusted code, so only a freely-loaded or consented extension
// is ever loaded (see Trust.h).
struct Panel
{
    std::string id; // capability id, default `<ext-id>:<index>`
    std::string label;
    std::string qml;
};

// A full-size dialog/overlay the extension contributes, the dialog counterpart
// to a Panel: where a Panel sits in the ~100px inspector slot, a Dialog is a
// modal overlay surface (the AI-tool conversions' original UX). `qml` names a
// file beside the manifest, the same bare-filename rule as panels.
// `menu_path`, when set, names the menu path the dialog's menu entry appears
// under; absent, the entry appears under the host's Extensions menu.
struct Dialog
{
    std::string id; // capability id, default `<ext-id>:<index>`
    std::string label;
    std::string qml;
    std::optional<std::string> menu_path;
};

// A QML widget the extension embeds into a named host surface (a `slot`
// contribution). `point` names one of the documented host surfaces
// (`clip.inspector`, `status.bar`, `settings.updates` - see docs/extensions.md);
// an unknown point refuses the manifest. `qml` names a file beside the
// manifest, the same bare-filename rule as panels/dialogs. `priority` orders
// the contribution within its point: a whole number 0..100, default 50, where
// a lower value renders earlier.
struct Slot
{
    std::string point;
    std::string qml;
    std::int32_t priority = 50;
};

// The capability a native extension contributes and that an `overrides` entry
// names. `Action` is a menu entry (the command it surfaces); `Panel` and
// `Dialog` are the QML surfaces; `Effect` names an effect-pack id (e.g.
// `genesis.glow` or `genesis.packs:genesis.glow`). The slot capability is
// deliberately excluded: slots are ordered, non-exclusive contributions, so
// there is nothing for an override to replace.
enum class CapabilityKind { Action, Panel, Dialog, Effect };

// One `<kind>:<id>` override declaration: the extension replaces the capability
// `id` (of `kind`) another extension contributes. `id` is the other
// extension's capability id, in either the namespaced or the default
// `<ext-id>:<index>` form. When kind is `Effect`, `id` is an effect-pack id
// (either the bare `genesis.<name>` id or `genesis.packs:genesis.<name>`).
struct OverrideEntry
{
    CapabilityKind kind;
    std::string id;
};

// A native extension's manifest (extension.toml): identity, the
// shared-library entry (absent for a data-only extension that contributes
// packs/panels but no library), and the capabilities it contributes. Parsing
// reads no native code and touches no file beyond the manifest itself; the
// library is loaded separately by NativeExtension, after trust and consent.
struct ExtensionManifest
{
    std::string id; // namespaced `author.name`, lower-case
    std::string name;
    std::uint32_t version = 1;
    std::uint32_t api = 1; // the API version the library was built against
    // Shared-library filename, beside the manifest. Absent for a data-only
    // extension: one that contributes packs/panels but no library. Such a
    // manifest must not contribute a menu `action` (which runs the library).
    // The PRESENCE of `entry` is the ONLY signal for "carries native code" —
    // there is no package-level kind differentiation.
    std::optional<std::string> entry;
    std::vector<MenuEntry> menus;
    std::vector<Panel> panels;
    std::vector<Dialog> dialogs;
    // The `[[slot]]` tables, in manifest order. Named `slot_entries` rather
    // than `slots` because the manifest is host-side but included from Qt
    // translation units, where `slots` is a Qt keyword macro.
    std::vector<Slot> slot_entries;
    // The effect contributions from `[[effect]]` blocks, in manifest order.
    std::vector<genesis::effects::Pack> effects;
    std::vector<std::string> packs; // pack ids this extension bundles
    std::vector<std::string> dependencies; // extension ids this one depends on
    // (the `requires` manifest key)
    // The `overrides` declarations: `<kind>:<id>` entries naming another
    // extension's capability this one replaces. Trust-gated (see Trust.h's
    // `may_override`): Builtin, Curated and Developer may declare them;
    // User is refused.
    std::vector<OverrideEntry> overrides;
};

// What went wrong reading a native manifest.
struct NativeLoadError
{
    std::string where; // the offending key/table, for a human
    std::string what; // what was wrong
};

// The manifest and the problems found. A non-empty `problems` means the
// manifest is unusable; an empty one means it validated.
struct NativeLoadResult
{
    std::optional<ExtensionManifest> manifest;
    std::vector<NativeLoadError> problems;
};

// Reads an extension.toml into the host schema.
NativeLoadResult load_native_manifest(std::string_view toml_text);

// Convenience over a path; reports I/O failures the same way.
NativeLoadResult load_native_manifest_file(const std::filesystem::path &path);

// Validates a native manifest for internal consistency, empty for a sound one.
std::vector<std::string> validate_native(const ExtensionManifest &manifest);

} // namespace genesis::extensions
