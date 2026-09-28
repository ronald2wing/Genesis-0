// SPDX-License-Identifier: GPL-3.0-or-later
// SPDX-FileCopyrightText: 2026 Genesis-0 contributors

#include <algorithm>
#include <cstdio>
#include <filesystem>
#include <set>
#include <string>
#include <string_view>
#include <system_error>
#include <vector>

#include "../support/Checks.h"
#include "effects/PackLoader.h"
#include "effects/PackSource.h"
#include "extensions/ExtensionManifest.h"
#include "effects/Resolve.h"
#include "project/model/Effects.h"

namespace {

using genesis::test::check;
using namespace genesis::effects;
using genesis::core::ShaderPass;
using genesis::project::AppliedFilter;

// The batch this test was written for: the thirty-six single-texture packs plus
// the five named-intermediate re-authors, so the shipped catalogue is complete
// at forty-one, grown by the four audio packs.
constexpr std::size_t kExpectedPacks = 123;

// The prelude the engine emits before a body - byte-for-byte the constant in
// src/adapters/engine/ges/effects/ShaderChain.cpp, copied here because the adapter is
// not linkable in a host-only test.
constexpr std::string_view kPrelude =
        "precision mediump float; varying vec2 v_texcoord; uniform sampler2D tex; ";

// A mirror of ShaderChain.cpp's `declares_sampler`: a body naming `sampler2D`
// is a second texture unit and is refused rather than emitted.
bool declares_sampler(std::string_view body)
{
    return body.find("sampler2D") != std::string_view::npos;
}

// How many times `needle` appears in `hay`.
std::size_t count_of(std::string_view hay, std::string_view needle)
{
    std::size_t total = 0;
    for (std::size_t at = hay.find(needle); at != std::string_view::npos;
         at = hay.find(needle, at + needle.size())) {
        ++total;
    }
    return total;
}

// The emitted fragment as ShaderChain.cpp composes it: the prelude, then one
// uniform declaration per field, then the body and a trailing newline.
std::string emit(const ShaderPass &pass)
{
    std::string source(kPrelude);
    for (const auto &field : pass.fields) {
        source += "uniform ";
        source += field.type;
        source += ' ';
        source += field.name;
        source += ";\n";
    }
    source += *pass.source;
    source += '\n';
    return source;
}

// The unified extension.toml for the builtin genesis.packs extension, found by
// walking up from the working directory.
std::optional<std::filesystem::path> locate_manifest()
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
            return std::nullopt;
        }
        dir = parent;
    }
}

// Loads every pack and resolves every pack, then checks the batch as a whole:
// the count is the expected one, the ids are unique, and no body does any of
// the things the engine refuses.
void test_the_batch_holds_thirty_six_clean_packs()
{
    const std::optional<std::filesystem::path> manifest = locate_manifest();
    check(manifest.has_value(), "extension.toml is found by walking up from cwd");
    if (!manifest) {
        return;
    }
    const genesis::extensions::NativeLoadResult loaded =
            genesis::extensions::load_native_manifest_file(*manifest);
    check(loaded.manifest.has_value(), "the manifest loads");
    if (!loaded.manifest) {
        return;
    }
    const std::vector<genesis::effects::Pack> &effects = loaded.manifest->effects;
    check(effects.size() == kExpectedPacks,
          "the pack count is " + std::to_string(kExpectedPacks) + " (saw "
                  + std::to_string(effects.size()) + ")");
    if (effects.empty()) {
        return;
    }

    const std::filesystem::path manifest_dir = manifest->parent_path();
    std::set<std::string> ids;
    for (const genesis::effects::Pack &pack : effects) {
        const std::string tag = pack.id;

        check(ids.insert(pack.id).second, tag + ": the id is unique across the batch");

        // A Pack declaring a [cpu] fallback draws named intermediates and ships
        // no GLSL body - the engine runs the element instead. Its fallback is
        // exercised by the resolve/fallback suites, not the body checks below.
        if (pack.cpu) {
            check(!is_expressible(pack),
                  tag + ": a cpu-fallback pack is not expressible on the GPU path");
            continue;
        }

        // An audio Pack ships no GLSL body - it drives a stock GStreamer audio
        // element through the `[audio]` backend. Its backend is exercised by
        // the resolve/audio suites, not the body checks below.
        if (pack.kind == Kind::Audio) {
            check(pack.audio.has_value(), tag + ": an audio pack declares an [audio] backend");
            continue;
        }

        check(is_expressible(pack), tag + ": the pack is expressible");

        const std::filesystem::path body_path = manifest_dir / pack.body;
        const SourceResult resolved =
                resolve_body(body_path, pack, AppliedFilter::create(pack.id), 0.0);
        check(resolved.pass.has_value() && resolved.problem.empty(),
              tag + ": resolve_body produces a pass");
        if (!resolved.pass) {
            continue;
        }
        const ShaderPass &pass = *resolved.pass;
        check(pass.package == pack.id, tag + ": the pass carries the pack's id");
        if (pass.source == nullptr || pass.source->empty()) {
            check(false, tag + ": the body is non-empty");
            continue;
        }

        const std::string_view body = *pass.source;
        check(!declares_sampler(body), tag + ": the body declares no sampler2D");
        check(body.find("#version") == std::string_view::npos,
              tag + ": the body declares no version (ES 1.00 is implied)");
        check(body.find("void main") != std::string_view::npos, tag + ": the body has a main");
        check(body.find("gl_FragColor") != std::string_view::npos,
              tag + ": the body writes gl_FragColor");

        // The whole fragment as the engine composes it: prelude, uniforms,
        // body. The prelude is the only `sampler2D` in the source.
        const std::string fragment = emit(pass);
        check(fragment.rfind(kPrelude, 0) == 0, tag + ": the prelude leads");
        check(count_of(fragment, "sampler2D") == 1, tag + ": exactly one sampler2D, the prelude's");
    }
}

// The sampler mirror is what the per-body check above rests on: it flags the
// one thing a body must not do.
void test_the_sampler_mirror_flags_a_second_texture()
{
    check(declares_sampler("uniform sampler2D other;"),
          "a body naming sampler2D is refused by the mirror");
    check(!declares_sampler("gl_FragColor = texture2D (tex, v_texcoord);"),
          "a body using texture2D is not refused");
}

} // namespace

int main()
{
    test_the_batch_holds_thirty_six_clean_packs();
    test_the_sampler_mirror_flags_a_second_texture();

    return genesis::test::summary();
}
