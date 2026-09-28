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

#include "effects/PackLoader.h"
#include "effects/PackSource.h"
#include "effects/Resolve.h"
#include "project/model/Effects.h"

namespace {

int checks = 0;
int failures = 0;

void check(bool condition, const std::string &what)
{
    ++checks;
    if (!condition) {
        ++failures;
        std::printf("FAIL: %s\n", what.c_str());
    }
}

using namespace genesis::effects;
using genesis::core::ShaderPass;
using genesis::project::AppliedFilter;

// The batch this test was written for: the thirty-six single-texture packs plus
// the five named-intermediate re-authors, so the shipped catalogue is complete
// at forty-one, grown by the four audio packs.
constexpr std::size_t kExpectedPacks = 83;

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

// The builtin genesis.packs packs/ directory: `cwd/extensions/builtin/
// genesis.packs/packs`, or the nearest ancestor's, so the test runs from the
// repo root or a build directory alike.
std::optional<std::filesystem::path> locate_packs()
{
    std::filesystem::path dir = std::filesystem::current_path();
    while (true) {
        const std::filesystem::path candidate =
                dir / "extensions" / "builtin" / "genesis.packs" / "packs";
        std::error_code ec;
        if (std::filesystem::is_directory(candidate, ec) && !ec) {
            for (const auto &entry : std::filesystem::directory_iterator(candidate, ec)) {
                if (entry.is_directory(ec)
                    && std::filesystem::exists(entry.path() / "effect.toml")) {
                    return candidate;
                }
            }
        }
        const std::filesystem::path parent = dir.parent_path();
        if (parent == dir) {
            return std::nullopt;
        }
        dir = parent;
    }
}

// The pack directories under `root`: one subdirectory per pack, sorted, each
// holding an effect.toml.
std::vector<std::filesystem::path> pack_dirs(const std::filesystem::path &root)
{
    std::vector<std::filesystem::path> dirs;
    std::error_code ec;
    for (const auto &entry : std::filesystem::directory_iterator(root, ec)) {
        if (entry.is_directory(ec) && std::filesystem::exists(entry.path() / "effect.toml")) {
            dirs.push_back(entry.path());
        }
    }
    std::sort(dirs.begin(), dirs.end());
    return dirs;
}

// Loads every pack and resolves every pack, then checks the batch as a whole:
// the count is the expected one, the ids are unique, and no body does any of
// the things the engine refuses.
void test_the_batch_holds_thirty_six_clean_packs()
{
    const std::optional<std::filesystem::path> root = locate_packs();
    check(root.has_value(), "packs/ is found by walking up from cwd");
    if (!root) {
        return;
    }
    const std::vector<std::filesystem::path> dirs = pack_dirs(*root);
    check(dirs.size() == kExpectedPacks,
          "the pack count is " + std::to_string(kExpectedPacks) + " (saw "
                  + std::to_string(dirs.size()) + ")");
    if (dirs.empty()) {
        return;
    }

    std::set<std::string> ids;
    for (const std::filesystem::path &dir : dirs) {
        const std::string tag = dir.filename().string();

        const LoadResult loaded = load_pack_file(dir / "effect.toml");
        check(loaded.problems.empty() && loaded.pack.has_value(),
              tag + ": the manifest loads clean");
        if (!loaded.pack) {
            continue;
        }
        check(ids.insert(loaded.pack->id).second, tag + ": the id is unique across the batch");

        // A Pack declaring a [cpu] fallback draws named intermediates and ships
        // no GLSL body - the engine runs the element instead. Its fallback is
        // exercised by the resolve/fallback suites, not the body checks below.
        if (loaded.pack->cpu) {
            check(!is_expressible(*loaded.pack),
                  tag + ": a cpu-fallback pack is not expressible on the GPU path");
            continue;
        }

        // An audio Pack ships no GLSL body - it drives a stock GStreamer audio
        // element through the `[audio]` backend. Its backend is exercised by
        // the resolve/audio suites, not the body checks below.
        if (loaded.pack->kind == Kind::Audio) {
            check(loaded.pack->audio.has_value(),
                  tag + ": an audio pack declares an [audio] backend");
            continue;
        }

        check(is_expressible(*loaded.pack), tag + ": the pack is expressible");

        const SourceResult resolved =
                resolve_source(dir, AppliedFilter::create(loaded.pack->id), 0.0);
        check(resolved.pass.has_value() && resolved.problem.empty(),
              tag + ": resolve_source produces a pass");
        if (!resolved.pass) {
            continue;
        }
        const ShaderPass &pass = *resolved.pass;
        check(pass.package == loaded.pack->id, tag + ": the pass carries the pack's id");
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

    std::printf("%d checks, %d failures\n", checks, failures);
    return failures == 0 ? 0 : 1;
}
