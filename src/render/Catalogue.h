// SPDX-License-Identifier: GPL-3.0-or-later
// SPDX-FileCopyrightText: 2026 Genesis-0 contributors

#pragma once

#include <filesystem>
#include <functional>
#include <memory>
#include <optional>
#include <string>
#include <unordered_map>
#include <vector>

#include <vector>

#include "effects/Pack.h"
#include "extensions/ExtensionHost.h"
#include "project/model/Clip.h"
#include "render/EffectFallback.h"

// The effect catalogue: the installed Packs a clip's effect chain resolves
// against. Engine-free by rule - no GES type appears here; the catalogue only
// answers "which Pack is this id?" and "what passes does this chain resolve
// to?" (docs/decisions/effect-execution.md §7.1).
//
// Where packs are found: an installed-pack store plus the builtin
// genesis.packs extension's source packs/ directory, so a developer build
// works before anything is installed. Roots
// are searched in order; a pack is identified by the `author.name` id its
// manifest declares. Read once and cached: a manifest and its body do not
// change under a running editor.

namespace genesis::render {

class Catalogue
{
public:
    // Builds the catalogue from the [[effect]] blocks in a single
    // extension.toml. The manifest is parsed by the extension loader;
    // each effect's `body` field (relative to the manifest directory) names
    // its GLSL source. Packs without a `body` (audio/cpu packs) are included
    // but have no source path.
    static Catalogue from_manifest(const std::filesystem::path &manifest_path);

    // Builds the catalogue from the ExtensionHost's loaded records: each
    // record's `effects` vector (the parsed [[effect]] blocks) is included,
    // and each effect's `body` is resolved relative to the record's installed
    // root. A pack's `body` is the field in the unified manifest; it is
    // resolved against `record.root` (the installed store directory), the
    // same base `from_manifest` uses.
    static Catalogue from_records(const std::vector<genesis::extensions::ExtensionRecord> &records);

    // The Pack answering to `id`, or null when no root holds one. A pointer
    // into the catalogue's own storage: stable for the catalogue's lifetime
    // and never copied, so the per-frame resolve does not deep-copy a Pack.
    const genesis::effects::Pack *pack(const std::string &id) const;

    // Every loaded Pack, optionally restricted to one kind (by its manifest
    // name, e.g. "filter"). Unordered, like the id map it is drawn from.
    std::vector<genesis::effects::Pack>
    packs(const std::optional<std::string> &kind = std::nullopt) const;

    // The passes a clip's enabled effect chain resolves to at `at`, skipping
    // and reporting every link that cannot resolve - the call `passes_at`
    // awaits. The clip's disabled links are bypassed here, not reported.
    ChainResolution passes_for(const genesis::project::Clip &clip, double at) const;

    // The same resolution with an injected element-availability probe: a link
    // the GPU path cannot express but which declares a CPU fallback is emitted
    // into the resolution's `cpu` list - instead of an R9 skip - when
    // `available` reports its element exists. An empty `available` preserves
    // the engine-free behaviour above (the fallback is never checked).
    ChainResolution passes_for(const genesis::project::Clip &clip, double at,
                               const std::function<bool(const std::string &)> &available) const;

    // The builtin catalogue, from the conventional source root (the builtin
    // genesis.packs extension's extension.toml), built once and cached.
    //
    // This is the fallback for a caller with no injected catalogue: the engine
    // adapter (GesBuilder, ClipEffects) and the CLI's `services.catalogue` are
    // engine-free/Qt-free and cannot hold a ExtensionHost, so they resolve the
    // builtin packs from the source tree. The app does NOT use this path - it
    // builds its catalogue from ExtensionHost records (from_records) and only
    // falls back here before the store is first seeded.
    static const Catalogue &builtin();

    // The conventional source manifest: the builtin genesis.packs extension's
    // extension.toml in the source tree, found by walking up from the working
    // directory, or an empty path when none is found. The builtin catalogue is
    // built over it via from_manifest. Kept (not deleted) because builtin()
    // backs the host-less callers above; the app's own path is from_records.
    static std::filesystem::path source_root();

private:
    // Pack id -> manifest.
    std::unordered_map<std::string, genesis::effects::Pack> packs_;
    // Pack id -> the absolute path to its effect.glsl body, for packs loaded
    // from a unified manifest (from_manifest / from_records) where `body`
    // names a path relative to the manifest or extension root.
    std::unordered_map<std::string, std::filesystem::path> body_paths_;
    // Pack id -> its effect.glsl body, read once and cached; nullptr means
    // "no body" and is cached too, so a missing file is not re-probed every
    // frame. Lazy and mutable because passes_for is const and runs on the
    // streaming thread; safe because every pack a treatment uses is resolved
    // at build time (single-threaded) before streaming begins, and a body
    // does not change under a running editor.
    mutable std::unordered_map<std::string, std::shared_ptr<const std::string>> bodies_;
};

} // namespace genesis::render
