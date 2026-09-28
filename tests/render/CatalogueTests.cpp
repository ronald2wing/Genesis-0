// SPDX-License-Identifier: GPL-3.0-or-later
// SPDX-FileCopyrightText: 2026 Genesis-0 contributors

#include <cstdio>
#include <filesystem>
#include <optional>
#include <span>
#include <string>
#include <system_error>
#include <utility>
#include <vector>

#include "project/command/ClipCommands.h"
#include "project/command/Command.h"
#include "project/command/MediaCommands.h"
#include "project/command/PropertyCommands.h"
#include "project/model/Clip.h"
#include "project/model/Effects.h"
#include "project/model/Media.h"
#include "project/model/Speed.h"
#include "project/Editor.h"
#include "render/Catalogue.h"
#include "render/Flatten.h"
#include "render/Resolve.h"

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

using namespace genesis::project;
namespace render = genesis::render;

// The source packs/ directory (the builtin genesis.packs extension's packs),
// found by walking up from the working directory - the same walk the
// pack-source tests use, because the build runs from anywhere under the tree.
std::optional<std::filesystem::path> locate_packs()
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
            return std::nullopt;
        }
        dir = parent;
    }
}

// A project with one video media item and one clip of it, built through the
// real command layer so the fixture cannot drift from the model.
struct Fixture
{
    Editor editor;
    std::string media_id;
    std::string clip_id;
};

Fixture fixture()
{
    Fixture f;
    NewMedia item;
    item.path = "/footage/a.mp4";
    item.name = "a.mp4";
    item.duration = Rational{ 10, 1 };
    item.kind = MediaKind::Video;
    item.width = 1920;
    item.height = 1080;
    item.has_audio = true;
    const auto add_media = f.editor.apply(Command{ AddMedia{ std::move(item) } });
    check(add_media.has_value(), "adds media");
    check(add_media->created_id.has_value(), "mints an id");
    f.media_id = *add_media->created_id;
    const std::string track_id = f.editor.project().timelines[0].tracks[0].id;
    const auto add_clip =
            f.editor.apply(Command{ AddClip{ f.media_id, track_id, Rational{ 1, 1 }, false } });
    check(add_clip.has_value(), "adds clip");
    check(add_clip->created_id.has_value(), "mints an id");
    f.clip_id = *add_clip->created_id;
    return f;
}

// A bare clip carrying `effects` - what passes_at hands the catalogue.
Clip clip_with(std::string id, std::vector<AppliedFilter> effects)
{
    Clip clip;
    clip.id = std::move(id);
    clip.video_effects = std::move(effects);
    return clip;
}

void test_one_enabled_effect_resolves_to_a_pass_with_a_body()
{
    const std::optional<std::filesystem::path> root = locate_packs();
    if (!root) {
        check(false, "packs/ found");
        return;
    }
    const render::Catalogue catalogue = render::Catalogue::from_directories({ *root });
    const Clip clip = clip_with("c1", { AppliedFilter::create("genesis.invert") });
    const render::ChainResolution result = catalogue.passes_for(clip, 0.0);
    check(result.passes.size() == 1, "one enabled effect resolves one pass");
    check(result.skips.empty(), "no link is skipped");
    if (!result.passes.empty()) {
        check(result.passes[0].package == "genesis.invert", "the pass carries the pack's id");
        check(result.passes[0].source != nullptr && !result.passes[0].source->empty(),
              "the pass's body is read from effect.glsl");
    }
}

void test_an_unknown_pack_is_skipped_and_reported()
{
    const std::optional<std::filesystem::path> root = locate_packs();
    if (!root) {
        check(false, "packs/ found");
        return;
    }
    const render::Catalogue catalogue = render::Catalogue::from_directories({ *root });
    const Clip clip = clip_with("c1", { AppliedFilter::create("no.such.pack") });
    const Clip before = clip;
    const render::ChainResolution result = catalogue.passes_for(clip, 0.0);
    check(result.passes.empty(), "an unknown pack resolves no pass");
    check(result.skips.size() == 1, "the unknown link is reported once");
    if (!result.skips.empty()) {
        check(result.skips[0].pack_id == "no.such.pack", "the skip names the id");
        check(result.skips[0].reason == "unknown pack", "the skip gives the reason");
    }
    check(clip == before, "R9: the clip is left unchanged");
}

void test_a_disabled_link_is_bypassed_not_reported()
{
    const std::optional<std::filesystem::path> root = locate_packs();
    if (!root) {
        check(false, "packs/ found");
        return;
    }
    const render::Catalogue catalogue = render::Catalogue::from_directories({ *root });
    AppliedFilter off = AppliedFilter::create("genesis.invert");
    off.enabled = false;
    const Clip clip = clip_with("c1", { std::move(off) });
    const render::ChainResolution result = catalogue.passes_for(clip, 0.0);
    check(result.passes.empty(), "a disabled link resolves no pass");
    check(result.skips.empty(), "a disabled link is not a failure");
}

void test_a_mixed_chain_reports_the_pass_and_the_skip()
{
    const std::optional<std::filesystem::path> root = locate_packs();
    if (!root) {
        check(false, "packs/ found");
        return;
    }
    const render::Catalogue catalogue = render::Catalogue::from_directories({ *root });
    const Clip clip = clip_with(
            "c1",
            { AppliedFilter::create("genesis.invert"), AppliedFilter::create("no.such.pack") });
    const render::ChainResolution result = catalogue.passes_for(clip, 0.0);
    check(result.passes.size() == 1, "the resolvable link resolves");
    check(result.skips.size() == 1, "the unresolvable link is skipped");
    if (!result.skips.empty()) {
        check(result.skips[0].pack_id == "no.such.pack", "the skip names the unknown id");
    }
}

void test_a_clip_with_a_transition_builds_a_span()
{
    Fixture f = fixture();
    ClipPatch patch;
    patch.transition_in = std::optional<Transition>{ Transition{ "cross-fade", Rational{ 1, 1 } } };
    check(f.editor.apply(UpdateClip{ f.clip_id, std::move(patch) }).has_value(),
          "sets a transition on the clip");

    const std::vector<render::ExportClip> flat =
            render::flatten_timeline(f.editor.project(), std::nullopt);
    check(flat.size() == 1, "one clip flattens");

    render::ExportRequest request;
    request.width = 1920;
    request.height = 1080;
    const render::BuiltTimeline built =
            render::build_timeline(request, genesis::core::FrameRate::THIRTY,
                                   std::span<const render::ExportClip>(flat), { });

    check(built.transitions.size() == 1, "one span is derived from the clip");
    if (built.transitions.size() == 1) {
        const render::TransitionSpan &span = built.transitions[0];
        check(span.id == "cross-fade", "the span carries the transition id");
        check(span.to_track == 0, "the span names the incoming clip's track");
        check(span.start == Rational{ 1, 1 }, "the span starts at the cut");
        check(span.end == Rational{ 2, 1 }, "the span runs the transition's duration");
    }
}

void test_a_project_with_neither_effects_nor_transitions_is_empty()
{
    Fixture f = fixture();
    const std::vector<render::ExportClip> flat =
            render::flatten_timeline(f.editor.project(), std::nullopt);
    render::ExportRequest request;
    request.width = 1920;
    request.height = 1080;
    const render::BuiltTimeline built =
            render::build_timeline(request, genesis::core::FrameRate::THIRTY,
                                   std::span<const render::ExportClip>(flat), { });

    check(built.treatments.empty(), "no treatments");
    check(built.transitions.empty(), "no transitions");
    check(built.chains.empty(), "no effect chains");
}

} // namespace

int main()
{
    test_one_enabled_effect_resolves_to_a_pass_with_a_body();
    test_an_unknown_pack_is_skipped_and_reported();
    test_a_disabled_link_is_bypassed_not_reported();
    test_a_mixed_chain_reports_the_pass_and_the_skip();
    test_a_clip_with_a_transition_builds_a_span();
    test_a_project_with_neither_effects_nor_transitions_is_empty();

    std::printf("%d checks, %d failures\n", checks, failures);
    return failures == 0 ? 0 : 1;
}
