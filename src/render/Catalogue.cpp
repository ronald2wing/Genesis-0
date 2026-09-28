// SPDX-License-Identifier: GPL-3.0-or-later
// SPDX-FileCopyrightText: 2026 Genesis-0 contributors

#include "render/Catalogue.h"

#include <fstream>
#include <memory>
#include <system_error>
#include <utility>

#include "extensions/ExtensionHost.h"

namespace genesis::render {

// The conventional source root: the builtin `genesis.packs` extension's
// manifest, found by walking up from the working directory. A developer build
// that has not seeded the extension store yet uses this as the only source.
std::filesystem::path Catalogue::source_root()
{
    std::filesystem::path dir = std::filesystem::current_path();
    while (true) {
        const std::filesystem::path candidate =
                dir / "extensions" / "builtin" / "genesis.packs" / "extension.toml";
        std::error_code ec;
        if (std::filesystem::is_regular_file(candidate, ec) && !ec) {
            return candidate;
        }
        const std::filesystem::path parent = dir.parent_path();
        if (parent == dir) {
            return { };
        }
        dir = parent;
    }
}

Catalogue Catalogue::from_manifest(const std::filesystem::path &manifest_path)
{
    Catalogue catalogue;
    const genesis::extensions::NativeLoadResult loaded =
            genesis::extensions::load_native_manifest_file(manifest_path);
    if (!loaded.manifest) {
        return catalogue;
    }
    const std::filesystem::path manifest_dir = manifest_path.parent_path();
    for (const genesis::effects::Pack &effect : loaded.manifest->effects) {
        // emplace keeps the first occurrence; duplicates are benign here
        // because the unified manifest has no duplicates.
        catalogue.packs_.emplace(effect.id, effect);
        if (!effect.body.empty()) {
            std::error_code ec;
            const std::filesystem::path abs_body =
                    std::filesystem::absolute(manifest_dir / effect.body, ec);
            if (!ec) {
                catalogue.body_paths_.emplace(effect.id, abs_body);
            }
        }
    }
    return catalogue;
}

Catalogue Catalogue::from_records(const std::vector<genesis::extensions::ExtensionRecord> &records)
{
    // The trust rank of an origin, so collision resolution favours the higher
    // trust when neither record explicitly overrides an effect id.
    const auto trust_rank = [](genesis::extensions::Origin origin) -> int {
        using genesis::extensions::Origin;
        switch (origin) {
        case Origin::Builtin:
            return 3;
        case Origin::Curated:
            return 2;
        case Origin::Developer:
            return 1;
        case Origin::User:
            return 0;
        }
        return 0;
    };

    Catalogue catalogue;
    // Track the origin of the winning record for each effect id so collision
    // resolution can compare ranks when neither side declares an override.
    std::unordered_map<std::string, genesis::extensions::Origin> winning_origin;

    for (const genesis::extensions::ExtensionRecord &record : records) {
        for (const genesis::effects::Pack &effect : record.effects) {
            const auto existing = catalogue.packs_.find(effect.id);
            if (existing != catalogue.packs_.end()) {
                // Does this record declare an `effect:<id>` override? If so it
                // wins regardless of trust rank — the override is its declared
                // intent to replace the effect pack.
                bool new_overrides = false;
                for (const genesis::extensions::OverrideEntry &ov : record.overrides) {
                    if (ov.kind == genesis::extensions::CapabilityKind::Effect
                        && ov.id == effect.id) {
                        new_overrides = true;
                        break;
                    }
                }
                if (!new_overrides) {
                    // No explicit override: the higher-trust origin wins the
                    // collision (Builtin > Curated > Developer > User), and
                    // the first-loaded keeps the id on a tie. Records are in
                    // topological order with builtins first, so the existing
                    // entry is already the highest-trust one — a lower-trust
                    // later record without an override is ignored.
                    if (trust_rank(record.origin) <= trust_rank(winning_origin[effect.id])) {
                        continue;
                    }
                }
            }
            catalogue.packs_[effect.id] = effect;
            winning_origin[effect.id] = record.origin;
            catalogue.body_paths_.erase(effect.id);
            if (!effect.body.empty()) {
                std::error_code ec;
                const std::filesystem::path abs_body =
                        std::filesystem::absolute(record.root / effect.body, ec);
                if (!ec) {
                    catalogue.body_paths_[effect.id] = abs_body;
                }
            }
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

std::vector<genesis::effects::Pack> Catalogue::packs(const std::optional<std::string> &kind) const
{
    std::vector<genesis::effects::Pack> out;
    out.reserve(packs_.size());
    for (const auto &[id, pack] : packs_) {
        if (!kind || genesis::effects::kind_name(pack.kind) == *kind) {
            out.push_back(pack);
        }
    }
    return out;
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
    // resolved body_paths_ entry. A pack whose body is missing keeps the pass
    // bodyless - the chain still resolves, the adapter simply has no GLSL to
    // compile. The read is cached per pack id (a missing body is cached too): a manifest
    // and its body do not change under a running editor, so the first resolve
    // is the only one that touches disk.
    for (genesis::core::ShaderPass &pass : result.passes) {
        const auto cached = bodies_.find(pass.package);
        if (cached != bodies_.end()) {
            pass.source = cached->second;
            continue;
        }
        std::shared_ptr<const std::string> body;
        const auto fpath = body_paths_.find(pass.package);
        if (fpath != body_paths_.end()) {
            std::ifstream in(fpath->second, std::ios::binary);
            if (in) {
                const std::string source((std::istreambuf_iterator<char>(in)),
                                         std::istreambuf_iterator<char>());
                if (!source.empty()) {
                    body = std::make_shared<const std::string>(source);
                }
            }
        }
        pass.source = body;
        bodies_.emplace(pass.package, std::move(body));
    }
    return result;
}

const Catalogue &Catalogue::builtin()
{
    static const Catalogue catalogue = Catalogue::from_manifest(Catalogue::source_root());
    return catalogue;
}

} // namespace genesis::render
