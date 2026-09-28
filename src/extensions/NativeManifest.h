// SPDX-License-Identifier: GPL-3.0-or-later
// SPDX-FileCopyrightText: 2026 Genesis-0 contributors

#pragma once

#include <cstdint>
#include <filesystem>
#include <optional>
#include <string>
#include <string_view>
#include <vector>

namespace genesis::extensions {

// One menu entry a native extension contributes: a command the host surfaces
// that either invokes the extension's `invoke(action)` or routes a declarative
// `call` to the host API through the injected bridge. Exactly one of `action`
// and `call` is set.
struct MenuEntry
{
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
    std::string label;
    std::string qml;
};

// A native extension's manifest (extension.toml, format 2; format 1 still
// reads): identity, the shared-library entry (absent for a data-only extension
// that contributes packs/panels but no library), and the capabilities it
// contributes. Parsing reads no native code and touches no file beyond the
// manifest itself; the library is loaded separately by NativeExtension, after
// trust and consent.
struct NativeManifest
{
    std::string id; // namespaced `author.name`, lower-case
    std::string name;
    std::uint32_t version = 1;
    std::uint32_t api = 1; // the API version the library was built against
    // Shared-library filename, beside the manifest. Absent for a data-only
    // extension: one that contributes packs/panels but no library. Such a
    // manifest must not contribute a menu `action` (which runs the library).
    std::optional<std::string> entry;
    std::vector<MenuEntry> menus;
    std::vector<Panel> panels;
    std::vector<std::string> packs; // pack ids this extension bundles
    std::vector<std::string> dependencies; // extension ids this one depends on
    // (the `requires` manifest key)
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
    std::optional<NativeManifest> manifest;
    std::vector<NativeLoadError> problems;
};

// Reads an extension.toml (format 2, or format 1) into the host schema.
NativeLoadResult load_native_manifest(std::string_view toml_text);

// Convenience over a path; reports I/O failures the same way.
NativeLoadResult load_native_manifest_file(const std::filesystem::path &path);

// Validates a native manifest for internal consistency, empty for a sound one.
std::vector<std::string> validate_native(const NativeManifest &manifest);

} // namespace genesis::extensions
