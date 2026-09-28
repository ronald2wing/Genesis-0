// SPDX-License-Identifier: GPL-3.0-or-later
// SPDX-FileCopyrightText: 2026 Genesis-0 contributors

#include "render/Catalogue.h"

#include <memory>
#include <system_error>
#include <utility>

#include "effects/PackLoader.h"
#include "effects/PackSource.h"

namespace genesis::render {

namespace {

// Every pack directory under `root`, in either layout a root may hold: a
// source packs/ directory (root/<id>/effect.toml) or an installed store
// (root/<id>/<version>/effect.toml). A child holding its own effect.toml is a
// pack; otherwise its children are checked one level deeper, which is the
// store's id/version nesting.
void collect_pack_dirs(const std::filesystem::path &root, std::vector<std::filesystem::path> &out)
{
    const std::filesystem::directory_iterator end;
    std::error_code ec;
    for (std::filesystem::directory_iterator it(root, ec); !ec && it != end; it.increment(ec)) {
        const std::filesystem::directory_entry entry = *it;
        std::error_code child_ec;
        if (!entry.is_directory(child_ec)) {
            continue;
        }
        std::error_code manifest_ec;
        if (std::filesystem::exists(entry.path() / "effect.toml", manifest_ec)) {
            out.push_back(entry.path());
            continue;
        }
        std::error_code inner_ec;
        for (std::filesystem::directory_iterator vit(entry.path(), inner_ec);
             !inner_ec && vit != end; vit.increment(inner_ec)) {
            const std::filesystem::directory_entry version = *vit;
            std::error_code version_ec;
            if (!version.is_directory(version_ec)) {
                continue;
            }
            std::error_code version_manifest_ec;
            if (std::filesystem::exists(version.path() / "effect.toml", version_manifest_ec)) {
                out.push_back(version.path());
            }
        }
    }
}

} // namespace

// The conventional source root: the builtin `genesis.packs` extension's packs
// directory in the source tree, found by walking up from the working
// directory. A developer build that has not seeded the extension store yet uses
// this as the only root - the same walk the pack-source tests use.
std::filesystem::path Catalogue::source_root()
{
    std::filesystem::path dir = std::filesystem::current_path();
    while (true) {
        const std::filesystem::path candidate =
                dir / "extensions" / "builtin" / "genesis.packs" / "packs";
        std::error_code ec;
        if (std::filesystem::is_directory(candidate, ec) && !ec) {
            return candidate;
        }
        const std::filesystem::path parent = dir.parent_path();
        if (parent == dir) {
            return { };
        }
        dir = parent;
    }
}

Catalogue Catalogue::from_directories(std::vector<std::filesystem::path> roots)
{
    Catalogue catalogue;
    for (const std::filesystem::path &root : roots) {
        std::error_code root_ec;
        if (!std::filesystem::is_directory(root, root_ec) || root_ec) {
            continue;
        }
        std::vector<std::filesystem::path> dirs;
        collect_pack_dirs(root, dirs);
        for (const std::filesystem::path &dir : dirs) {
            const genesis::effects::LoadResult loaded =
                    genesis::effects::load_pack_file(dir / "effect.toml");
            // A manifest that does not load is not a pack: reported by the
            // loader, it is simply not part of the catalogue.
            if (!loaded.pack) {
                continue;
            }
            // emplace keeps the first root holding an id; a later root does
            // not override it.
            catalogue.dirs_.emplace(loaded.pack->id, dir);
            catalogue.packs_.emplace(loaded.pack->id, std::move(*loaded.pack));
        }
    }
    return catalogue;
}

const genesis::effects::Pack *Catalogue::pack(const std::string &id) const
{
    const auto it = packs_.find(id);
    if (it == packs_.end()) {
        return nullptr;
    }
    return &it->second;
}

ChainResolution Catalogue::passes_for(const genesis::project::Clip &clip, double at) const
{
    return passes_for(clip, at, { });
}

ChainResolution
Catalogue::passes_for(const genesis::project::Clip &clip, double at,
                      const std::function<bool(const std::string &)> &available) const
{
    // A disabled link is a bypass, not a failure: it is dropped before
    // resolve_effect_chain sees it, so it never surfaces as an EffectSkip.
    genesis::project::Clip enabled = clip;
    enabled.video_effects.clear();
    enabled.video_effects.reserve(clip.video_effects.size());
    for (const genesis::project::AppliedFilter &filter : clip.video_effects) {
        if (filter.enabled) {
            enabled.video_effects.push_back(filter);
        }
    }

    ChainResolution result = resolve_effect_chain(
            enabled, at,
            [this](const std::string &id) -> const genesis::effects::Pack * { return pack(id); },
            available);

    // resolve_pass lays the params out but never reads the shader source; the
    // body is the engine's business and is read here, once, from the pack's
    // directory. A pack whose body is missing keeps the pass bodyless - the
    // chain still resolves, the adapter simply has no GLSL to compile. The
    // read is cached per pack id (a missing body is cached too): a manifest
    // and its body do not change under a running editor, so the first resolve
    // is the only one that touches disk.
    for (genesis::core::ShaderPass &pass : result.passes) {
        const auto cached = bodies_.find(pass.package);
        if (cached != bodies_.end()) {
            pass.source = cached->second;
            continue;
        }
        std::shared_ptr<const std::string> body;
        const auto dir = dirs_.find(pass.package);
        if (dir != dirs_.end()) {
            const genesis::effects::BodyResult read = genesis::effects::read_body(dir->second);
            if (read.body) {
                body = std::make_shared<const std::string>(std::move(*read.body));
            }
        }
        pass.source = body;
        bodies_.emplace(pass.package, std::move(body));
    }
    return result;
}

const Catalogue &Catalogue::builtin()
{
    static const Catalogue catalogue = Catalogue::from_directories({ Catalogue::source_root() });
    return catalogue;
}

} // namespace genesis::render
