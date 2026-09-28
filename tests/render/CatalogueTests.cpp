// SPDX-License-Identifier: GPL-3.0-or-later
// SPDX-FileCopyrightText: 2026 Genesis-0 contributors

#include <cstdio>
#include <filesystem>
#include <fstream>
#include <optional>
#include <span>
#include <string>
#include <system_error>
#include <utility>
#include <vector>

#include "../support/Checks.h"
#include "project/command/ClipCommands.h"
#include "project/command/Command.h"
#include "project/command/MediaCommands.h"
#include "project/command/PropertyCommands.h"
#include "project/model/Clip.h"
#include "project/model/Effects.h"
#include "project/model/Media.h"
#include "project/model/Speed.h"
#include "project/Editor.h"
#include "extensions/ExtensionHost.h"
#include "render/Catalogue.h"
#include "render/Flatten.h"
#include "render/Resolve.h"

namespace {

using genesis::test::check;
using namespace genesis::project;
namespace render = genesis::render;

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

void test_from_records_builds_from_extension_records()
{
    // Build a synthetic ExtensionRecord with one effect Pack whose `body`
    // names a GLSL file relative to the record's installed root. Write the
    // body to a temp directory so passes_for can resolve it.
    const std::filesystem::path root =
            std::filesystem::temp_directory_path() / "genesis-catalogue-records-test";
    std::filesystem::remove_all(root);
    std::filesystem::create_directories(root);
    {
        std::ofstream out(root / "test_body.glsl");
        out << "// test body";
    }

    genesis::extensions::ExtensionRecord record;
    record.root = root;
    genesis::effects::Pack pack;
    pack.id = "test.record.pack";
    pack.name = "Record Pack";
    pack.kind = genesis::effects::Kind::Filter;
    pack.body = "test_body.glsl";
    record.effects.push_back(pack);

    const render::Catalogue catalogue = render::Catalogue::from_records({ record });

    const genesis::effects::Pack *resolved = catalogue.pack("test.record.pack");
    check(resolved != nullptr, "from_records resolves the pack by id");
    if (resolved) {
        check(resolved->id == "test.record.pack", "the pack carries its id");
        check(resolved->kind == genesis::effects::Kind::Filter, "the pack carries its kind");
    }

    // passes_for resolves the body through body_paths_.
    const Clip clip = clip_with("c1", { AppliedFilter::create("test.record.pack") });
    const render::ChainResolution result = catalogue.passes_for(clip, 0.0);
    check(result.passes.size() == 1, "the record's pack resolves a pass");
    if (!result.passes.empty()) {
        check(result.passes[0].package == "test.record.pack", "the pass names the pack");
        check(result.passes[0].source != nullptr && !result.passes[0].source->empty(),
              "the body is read from the resolved path");
    }

    std::filesystem::remove_all(root);
}

void test_one_enabled_effect_resolves_to_a_pass_with_a_body()
{
    const std::optional<std::filesystem::path> root = locate_manifest();
    if (!root) {
        check(false, "packs/ found");
        return;
    }
    const render::Catalogue catalogue = render::Catalogue::from_manifest(*root);
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
    const std::optional<std::filesystem::path> root = locate_manifest();
    if (!root) {
        check(false, "packs/ found");
        return;
    }
    const render::Catalogue catalogue = render::Catalogue::from_manifest(*root);
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
    const std::optional<std::filesystem::path> root = locate_manifest();
    if (!root) {
        check(false, "packs/ found");
        return;
    }
    const render::Catalogue catalogue = render::Catalogue::from_manifest(*root);
    AppliedFilter off = AppliedFilter::create("genesis.invert");
    off.enabled = false;
    const Clip clip = clip_with("c1", { std::move(off) });
    const render::ChainResolution result = catalogue.passes_for(clip, 0.0);
    check(result.passes.empty(), "a disabled link resolves no pass");
    check(result.skips.empty(), "a disabled link is not a failure");
}

void test_a_mixed_chain_reports_the_pass_and_the_skip()
{
    const std::optional<std::filesystem::path> root = locate_manifest();
    if (!root) {
        check(false, "packs/ found");
        return;
    }
    const render::Catalogue catalogue = render::Catalogue::from_manifest(*root);
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

void test_packs_filter_by_kind()
{
    const std::optional<std::filesystem::path> root = locate_manifest();
    if (!root) {
        check(false, "packs/ found");
        return;
    }
    const render::Catalogue catalogue = render::Catalogue::from_manifest(*root);

    const auto all = catalogue.packs(std::nullopt);
    check(!all.empty(), "the catalogue loads packs");

    // The five kind names a manifest spells, each filtering to exactly that
    // kind - the names `Catalogue::packs(kind)` matches against, so this pins
    // the filter strings to `effects::Pack.h`'s `kind_name`. A kind with no
    // shipped pack (transition, generator) filters to empty, not an error.
    const std::vector<std::pair<std::string, genesis::effects::Kind>> kinds = {
        { "effect", genesis::effects::Kind::Effect },
        { "filter", genesis::effects::Kind::Filter },
        { "audio", genesis::effects::Kind::Audio },
        { "transition", genesis::effects::Kind::Transition },
        { "generator", genesis::effects::Kind::Generator },
    };
    std::size_t filtered_total = 0;
    for (const auto &[name, kind] : kinds) {
        const auto subset = catalogue.packs(name);
        for (const auto &pack : subset) {
            check(pack.kind == kind, "the '" + name + "' filter returns only that kind");
        }
        filtered_total += subset.size();
    }
    check(filtered_total == all.size(), "the five kind filters partition the catalogue");
    check(!catalogue.packs("effect").empty(), "the effect kind has packs");
    check(!catalogue.packs("filter").empty(), "the filter kind has packs");
    check(!catalogue.packs("audio").empty(), "the audio kind has packs");
}

void test_from_records_override_effect_wins()
{
    // Two records contribute the same effect id. The builtin record is first
    // (highest trust). The Developer record declares an `effect:<id>` override
    // and contributes a replacement pack: the Developer's pack must win.
    const std::filesystem::path builtin_root =
            std::filesystem::temp_directory_path() / "genesis-catalogue-override-builtin";
    const std::filesystem::path dev_root =
            std::filesystem::temp_directory_path() / "genesis-catalogue-override-dev";
    std::filesystem::remove_all(builtin_root);
    std::filesystem::remove_all(dev_root);
    std::filesystem::create_directories(builtin_root);
    std::filesystem::create_directories(dev_root);
    {
        std::ofstream out(builtin_root / "builtin_body.glsl");
        out << "// builtin body";
    }
    {
        std::ofstream out(dev_root / "dev_body.glsl");
        out << "// dev body";
    }

    genesis::extensions::ExtensionRecord builtin_rec;
    builtin_rec.root = builtin_root;
    builtin_rec.origin = genesis::extensions::Origin::Builtin;
    genesis::effects::Pack builtin_pack;
    builtin_pack.id = "genesis.glow";
    builtin_pack.name = "Glow";
    builtin_pack.kind = genesis::effects::Kind::Effect;
    builtin_pack.body = "builtin_body.glsl";
    builtin_rec.effects.push_back(builtin_pack);

    genesis::extensions::ExtensionRecord dev_rec;
    dev_rec.root = dev_root;
    dev_rec.origin = genesis::extensions::Origin::Developer;
    dev_rec.overrides.push_back(genesis::extensions::OverrideEntry{
            genesis::extensions::CapabilityKind::Effect, "genesis.glow" });
    genesis::effects::Pack dev_pack;
    dev_pack.id = "genesis.glow";
    dev_pack.name = "Glow Override";
    dev_pack.kind = genesis::effects::Kind::Effect;
    dev_pack.body = "dev_body.glsl";
    dev_rec.effects.push_back(dev_pack);

    const render::Catalogue catalogue = render::Catalogue::from_records({ builtin_rec, dev_rec });

    const genesis::effects::Pack *resolved = catalogue.pack("genesis.glow");
    check(resolved != nullptr, "the effect is resolved");
    if (resolved) {
        check(resolved->name == "Glow Override",
              "the Developer's overriding effect wins because it declared an override");
        check(resolved->kind == genesis::effects::Kind::Effect, "the kind round-trips");
    }

    // passes_for resolves the Developer's body, not the builtin's.
    const Clip clip = clip_with("c1", { AppliedFilter::create("genesis.glow") });
    const render::ChainResolution result = catalogue.passes_for(clip, 0.0);
    check(result.passes.size() == 1, "the overridden effect resolves a pass");
    if (!result.passes.empty()) {
        check(result.passes[0].source != nullptr && !result.passes[0].source->empty(),
              "the body is read from the overriding path");
        check(result.passes[0].source->find("dev body") != std::string::npos,
              "the body content is the Developer's, not the builtin's");
    }

    std::filesystem::remove_all(builtin_root);
    std::filesystem::remove_all(dev_root);
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
    test_from_records_builds_from_extension_records();
    test_from_records_override_effect_wins();
    test_one_enabled_effect_resolves_to_a_pass_with_a_body();
    test_an_unknown_pack_is_skipped_and_reported();
    test_a_disabled_link_is_bypassed_not_reported();
    test_a_mixed_chain_reports_the_pass_and_the_skip();
    test_a_clip_with_a_transition_builds_a_span();
    test_packs_filter_by_kind();
    test_a_project_with_neither_effects_nor_transitions_is_empty();

    return genesis::test::summary();
}
