// SPDX-License-Identifier: GPL-3.0-or-later
// SPDX-FileCopyrightText: 2026 Genesis-0 contributors

#include <algorithm>
#include <array>
#include <cstdint>
#include <cstdio>
#include <filesystem>
#include <optional>
#include <string>
#include <string_view>
#include <system_error>
#include <vector>

#include "core/ShaderPass.h"
#include "effects/PackLoader.h"
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

using genesis::core::RevealMap;
using genesis::effects::is_expressible;
using genesis::effects::load_pack;
using genesis::effects::load_pack_file;
using genesis::effects::LoadResult;
using genesis::effects::resolve_pass;
using genesis::project::AppliedFilter;

// --- The reveal predicate, in host code ------------------------------------
//
// A title's per-word reveal works by baking the read order of each word into
// one grayscale map - a byte per pixel, 0 for the first word, 255 for the last
// (core/ShaderPass.h `RevealMap`) - and showing a pixel when its word's order
// is at or below `progress`. The GLSL body samples the map and does one
// comparison; this host copy is the same arithmetic, so the ordering contract
// is tested without a GPU.
//
// `gray` is the byte the map stores for a pixel; `progress` is 0..1. The first
// word reads 0 and shows from the first frame, the last reads 255 and shows
// only when progress reaches 1.
bool revealed(std::uint8_t gray, float progress)
{
    const float order = static_cast<float>(gray) / 255.0f;
    return order <= progress;
}

// A hand-built map, five pixels wide: two words of two pixels with a one-pixel
// gap, so the background (outside every rect) is exercised too. from_rects
// paints 0 for the first word, 255 for the last, and leaves the gap at 0 - the
// "already revealed" background ShaderPass.h documents.
void test_the_reveal_predicate_orders_two_words()
{
    const RevealMap map = RevealMap::from_rects(5, 1, { { { 0, 0, 2, 1 }, { 3, 0, 2, 1 } } });
    check(map.width == 5 && map.height == 1, "the hand-built map is 5 by 1");
    check(map.gray != nullptr && map.gray->size() == 5, "it holds five bytes");
    if (map.gray == nullptr || map.gray->size() != 5) {
        return;
    }

    const std::uint8_t first = (*map.gray)[0];
    const std::uint8_t background = (*map.gray)[2];
    const std::uint8_t last = (*map.gray)[3];
    check(first == 0, "the first word is order 0");
    check(background == 0, "the background is left already revealed (order 0)");
    check(last == 255, "the second word is order 255");

    // Before: at low progress only the first word (and the transparent
    // background) is shown. After: at progress 1 the last word shows too.
    check(revealed(first, 0.0f), "the first word shows at progress 0");
    check(revealed(background, 0.0f), "the background shows from the start");
    check(!revealed(last, 0.0f), "the late word is hidden at progress 0");
    check(!revealed(last, 0.5f), "the late word is still hidden at progress 0.5");
    check(revealed(last, 1.0f), "the late word shows at progress 1");
}

// --- The manifest and resolver contract ------------------------------------
//
// A reveal pack declares one animated `progress` parameter and no passes; the
// reveal map itself is a bound resource the app attaches to the pass, not a
// declared parameter. The text below is the manifest a shipped pack would
// carry, loaded through the same loader a file uses, so the contract is tested
// without shipping a pack whose body the stock glshader path cannot express
// (see the finding test below).

void test_a_reveal_manifest_loads_and_resolves()
{
    const LoadResult loaded = load_pack(R"(
        format = 2

        [effect]
        id = "genesis.reveal"
        name = "Reveal"
        kind = "effect"
        category = "Title"
        description = "Reveals a title's words one at a time."

        [[param]]
        key = "progress"
        label = "Progress"
        min = 0
        max = 1
        default = 0
        step = 0.01
        animate = true

        [wgsl]
        entry = "effect.glsl"
    )");
    check(loaded.problems.empty(), "a reveal manifest validates");
    check(loaded.pack.has_value(), "and loads");
    if (!loaded.pack) {
        return;
    }
    check(loaded.pack->id == "genesis.reveal", "the pack id is namespaced");
    check(is_expressible(*loaded.pack), "a single-pass reveal is expressible");
    check(loaded.pack->parameters.size() == 1 && loaded.pack->parameters[0].key == "progress",
          "it declares one progress parameter");

    const auto pass = resolve_pass(*loaded.pack, AppliedFilter::create("genesis.reveal"), 0.0);
    check(pass.has_value(), "the reveal pack resolves to a pass");
    if (!pass) {
        return;
    }
    check(pass->fields.size() == 1 && pass->fields[0].name == "progress"
                  && pass->fields[0].type == "float" && pass->fields[0].offset == 0
                  && pass->fields[0].size == 4,
          "progress is a float field at offset 0");
    // The resolver never sets reveal_map: the app attaches the baked map to the
    // pass before the adapter runs, and the adapter (stock glshader) cannot
    // bind a second texture anyway.
    check(!pass->reveal_map.has_value(),
          "the resolver leaves the reveal map for the app to attach");
}

// --- The finding: the reveal map cannot be bound ---------------------------
//
// A reveal pass must sample BOTH the title picture (the layer) and the reveal
// map (a second 2D texture). The stock glshader element binds exactly one
// texture - `tex`, unit 0 - and a second `sampler2D` silently aliases unit 0
// with no error (docs/decisions/effects-execution.md §6). A body that declares
// the map's sampler is therefore refused rather than emitted; the pass would
// otherwise read the picture back as if it were the order. This is the same
// settled negative as the named-intermediate packs, expressible only through
// the custom GstGLMixer element (spikes/gl-custom-element/).

// A mirror of ShaderChain.cpp's `declares_sampler`: the adapter is not linkable
// in a host-only test, so the refusal is mirrored here, byte-for-byte.
bool declares_sampler(std::string_view body)
{
    return body.find("sampler2D") != std::string_view::npos;
}

// The reveal body a naive author would write: it samples the map through a
// second sampler, which would silently alias unit 0.
constexpr std::string_view kNaiveRevealBody =
        "uniform sampler2D reveal_order;"
        " void main () {"
        " vec4 c = texture2D (tex, v_texcoord);"
        " float order = texture2D (reveal_order, v_texcoord).r;"
        " gl_FragColor = (order <= progress) ? c : vec4 (0.0); }";

void test_a_reveal_body_cannot_coexist_with_the_picture()
{
    check(declares_sampler(kNaiveRevealBody),
          "a reveal body declares a second sampler for the map");
    // The adapter refuses exactly this, so the reveal pass cannot coexist with
    // the picture: fragment_source_for returns nullopt rather than emitting a
    // shader that silently samples the wrong texture.
    check(declares_sampler("uniform sampler2D other;"),
          "any second sampler2D is refused (the silent-aliasing trap)");
    check(!declares_sampler("gl_FragColor = texture2D (tex, v_texcoord);"),
          "a single-texture body is not refused");
}

// --- The shipped packs still load ------------------------------------------

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

void test_the_existing_packs_still_load()
{
    const std::optional<std::filesystem::path> root = locate_packs();
    check(root.has_value(), "packs/ is found by walking up from cwd");
    if (!root) {
        return;
    }
    const std::vector<std::filesystem::path> dirs = pack_dirs(*root);
    check(dirs.size() == 83,
          "exactly 83 packs are shipped (saw " + std::to_string(dirs.size()) + ")");
    for (const std::filesystem::path &dir : dirs) {
        const LoadResult loaded = load_pack_file(dir / "effect.toml");
        check(loaded.problems.empty() && loaded.pack.has_value(),
              dir.filename().string() + ": loads clean");
    }
}

} // namespace

int main()
{
    test_the_reveal_predicate_orders_two_words();
    test_a_reveal_manifest_loads_and_resolves();
    test_a_reveal_body_cannot_coexist_with_the_picture();
    test_the_existing_packs_still_load();

    std::printf("%d checks, %d failures\n", checks, failures);
    return failures == 0 ? 0 : 1;
}
