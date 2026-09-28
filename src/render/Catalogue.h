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

#include "effects/Pack.h"
#include "project/model/Clip.h"
#include "render/EffectFallback.h"

// The effect catalogue: the installed Packs a clip's effect chain resolves
// against. Engine-free by rule - no GES type appears here; the catalogue only
// answers "which Pack is this id?" and "what passes does this chain resolve
// to?" (docs/decisions/effects-execution.md §7.1).
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
    // Builds the catalogue from the given root directories. A root is either a
    // source packs/ directory (root/<id>/effect.toml) or an installed store
    // (root/<id>/<version>/effect.toml); both layouts are walked. Earlier
    // roots win when two hold the same id.
    static Catalogue from_directories(std::vector<std::filesystem::path> roots);

    // The Pack answering to `id`, or null when no root holds one. A pointer
    // into the catalogue's own storage: stable for the catalogue's lifetime
    // and never copied, so the per-frame resolve does not deep-copy a Pack.
    const genesis::effects::Pack *pack(const std::string &id) const;

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
    // genesis.packs extension's packs/ directory), built once and cached.
    static const Catalogue &builtin();

    // The conventional source packs/ directory: the builtin genesis.packs
    // extension's packs/ directory in the source tree, found by walking up
    // from the working directory, or an empty path when none is found. The
    // builtin catalogue is built over it; a caller that wants to add
    // contributed pack roots builds its own catalogue over this path plus the
    // extra roots, via from_directories.
    static std::filesystem::path source_root();

private:
    // Pack id -> manifest.
    std::unordered_map<std::string, genesis::effects::Pack> packs_;
    // Pack id -> the directory its effect.toml and effect.glsl sit in, so a
    // resolved pass's body can be read back from where it came.
    std::unordered_map<std::string, std::filesystem::path> dirs_;
    // Pack id -> its effect.glsl body, read once and cached; nullptr means
    // "no body" and is cached too, so a missing file is not re-probed every
    // frame. Lazy and mutable because passes_for is const and runs on the
    // streaming thread; safe because every pack a treatment uses is resolved
    // at build time (single-threaded) before streaming begins, and a body
    // does not change under a running editor.
    mutable std::unordered_map<std::string, std::shared_ptr<const std::string>> bodies_;
};

} // namespace genesis::render
