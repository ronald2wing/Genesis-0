// SPDX-License-Identifier: GPL-3.0-or-later
// SPDX-FileCopyrightText: 2026 Genesis-0 contributors

#pragma once

#include <cstdint>
#include <filesystem>
#include <map>
#include <optional>
#include <string>
#include <vector>

#include "extensions/NativeExtension.h"
#include "extensions/NativeManifest.h"

namespace genesis::extensions {

// The load state of one installed native extension, as the app surfaces it.
enum class ExtensionState { Enabled, Disabled, Failed };

// One panel the host surfaces: the manifest's label and bare `qml` filename,
// plus the absolute file URL the QML loader opens. `url` is the qml filename
// joined to the extension root and percent-encoded (the same join rule the
// library entry uses); the manifest loader never fills it.
struct PanelEntry
{
    std::string label;
    std::string qml;
    std::string url;
};

// One installed native extension, as the app surfaces it: its manifest
// identity, its origin, the effective load state and the reason it is not
// enabled (empty when enabled), whether Developer consent is recorded, and -
// for an enabled extension - the contributions the host collected.
struct ExtensionRecord
{
    std::string id;
    std::string name;
    std::uint32_t version = 0;
    Origin origin = Origin::Developer;
    ExtensionState state = ExtensionState::Failed;
    std::string reason; // why failed/disabled; empty when enabled
    bool consented = false; // Developer consent recorded for this id
    std::filesystem::path root; // store_root/<id>/<version>, when installed
    // The extension ids this one declares it depends on (the manifest's
    // `requires`), in manifest order; and the installed ids that declare they
    // depend on this one (attached after the scan resolves the graph).
    std::vector<std::string> dependencies;
    std::vector<std::string> dependents;
    std::vector<MenuEntry> menus;
    std::vector<PanelEntry> panels;
    std::filesystem::path pack_root; // root/packs when it exists, else empty
};

// The Qt-free host that scans the installed-extension store, loads every
// loadable native extension, and collects their contributions. It owns the
// loaded libraries (which are never unloaded - see NativeExtension) and drives
// them from the main thread, in the style of jobs::Scheduler.
class ExtensionHost
{
public:
    // `store_root` is the directory installed extensions live under
    // (`<id>/<version>`). `host` binds the API bridge and the log sink; each
    // loaded library keeps its own copy, with the log sink re-prefixed
    // `[ext:<id>]` so a shared sink still names the extension that spoke.
    ExtensionHost(std::filesystem::path store_root, NativeExtension::Host host);

    // Re-scans the store and (re)loads. The highest version of each id is
    // loaded; a refusal (untrusted, no consent, bad manifest, bad library) is
    // collected as a `failed` record and logged, never fatal. A Builtin/Curated
    // id on the store's `disabled.json` list is collected as `disabled`
    // (visible but not loaded); Developer ids are gated by consent alone.
    void scan();

    // The loaded (enabled) extensions, with their contributions.
    const std::vector<ExtensionRecord> &extensions() const { return loaded_; }
    // Visible-but-disabled Builtin/Curated extensions (on the disabled list).
    const std::vector<ExtensionRecord> &disabled() const { return disabled_; }
    // Refused extensions (untrusted, no consent, bad manifest/library).
    const std::vector<ExtensionRecord> &failed() const { return failed_; }

    // Compatibility: one refusal string per failed record ("id: reason").
    const std::vector<std::string> &refusals() const { return refusals_; }

    // The origin recorded for `id`, or Developer when none is recorded (the
    // historical default for a store entry).
    Origin origin_of(const std::string &id) const;

    // Every contributed pack directory (each loaded extension's `root/packs`
    // that exists), in load order. The app builds its effect catalogue over
    // these roots alongside the builtin source root.
    std::vector<std::filesystem::path> pack_roots() const;

    // Invokes one loaded extension's action. Fails cleanly for an unknown id,
    // a data-only extension (which contributes no library), or an extension
    // that contributed no invoke.
    std::expected<std::string, std::string> invoke(const std::string &id, const std::string &action,
                                                   const std::string &args) const;

    void on_project_opened(const std::string &path) const;
    void shutdown() const;

private:
    std::filesystem::path store_root_;
    NativeExtension::Host host_;
    std::vector<ExtensionRecord> loaded_;
    std::vector<ExtensionRecord> disabled_;
    std::vector<ExtensionRecord> failed_;
    // Parallel to loaded_; owns the code extensions' handles. A data-only
    // extension loads as a record with no library, so its slot is nullopt.
    std::vector<std::optional<NativeExtension>> natives_;
    std::vector<std::string> refusals_;
    // id -> origin, read from `<store>/origins.json`; absent ids are Developer.
    std::map<std::string, Origin> origins_;
};

} // namespace genesis::extensions
