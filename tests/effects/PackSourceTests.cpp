// SPDX-License-Identifier: GPL-3.0-or-later
// SPDX-FileCopyrightText: 2026 Genesis-0 contributors

#include <algorithm>
#include <cstdio>
#include <filesystem>
#include <fstream>
#include <optional>
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

// A minimal valid manifest, for a synthetic pack directory.
constexpr const char *kMinimalManifest = R"(
[effect]
id = "genesis.test"
name = "Test"
kind = "effect"

[wgsl]
entry = "effect.wgsl"
)";

void test_the_packs_directory_holds_a_batch()
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
    check(loaded.manifest->effects.size() == 123,
          "the pack count is 123 (saw " + std::to_string(loaded.manifest->effects.size()) + ")");
}

void test_every_shipped_pack_resolves_to_a_clean_body()
{
    const std::optional<std::filesystem::path> manifest = locate_manifest();
    if (!manifest) {
        check(false, "extension.toml is found");
        return;
    }
    const genesis::extensions::NativeLoadResult loaded =
            genesis::extensions::load_native_manifest_file(*manifest);
    check(loaded.manifest.has_value(), "the manifest loads");
    if (!loaded.manifest) {
        return;
    }
    const std::filesystem::path manifest_dir = manifest->parent_path();
    check(!loaded.manifest->effects.empty(), "there is at least one pack to inspect");
    for (const genesis::effects::Pack &pack : loaded.manifest->effects) {
        const std::string tag = pack.id;

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

        // Resolve via the body path stored in the pack's manifest.
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

void test_a_wgsl_only_pack_reports_no_glsl_body()
{
    const std::filesystem::path dir =
            std::filesystem::temp_directory_path() / "genesis-wgsl-only-pack";
    std::error_code ec;
    std::filesystem::create_directories(dir, ec);
    {
        std::ofstream manifest(dir / "effect.toml");
        manifest << kMinimalManifest;
    }
    {
        std::ofstream wgsl(dir / "effect.wgsl");
        wgsl << "// a WGSL body is not a GLSL body\n";
    }

    const BodyResult body = read_body(dir);
    check(!body.body.has_value(), "a wgsl-only pack has no GLSL body");
    check(body.problem.find("no GLSL body") != std::string::npos, "and read_body says so");

    const SourceResult resolved = resolve_source(dir, AppliedFilter::create("genesis.test"), 0.0);
    check(!resolved.pass.has_value(), "resolve_source refuses a pack with no GLSL body");
    check(resolved.problem.find("no GLSL body") != std::string::npos,
          "and the body problem is carried through");

    std::filesystem::remove_all(dir, ec);
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
    test_the_packs_directory_holds_a_batch();
    test_every_shipped_pack_resolves_to_a_clean_body();
    test_a_wgsl_only_pack_reports_no_glsl_body();
    test_the_sampler_mirror_flags_a_second_texture();

    return genesis::test::summary();
}
