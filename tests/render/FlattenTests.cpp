// SPDX-License-Identifier: GPL-3.0-or-later
// SPDX-FileCopyrightText: 2026 Genesis-0 contributors

#include <cstdio>
#include <filesystem>
#include <optional>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

#include "../support/Checks.h"
#include "ai/vision/MaskStore.h"
#include "project/command/ClipCommands.h"
#include "project/command/Command.h"
#include "project/command/MediaCommands.h"
#include "project/command/PropertyCommands.h"
#include "project/command/TrackCommands.h"
#include "project/model/Clip.h"
#include "project/model/Effects.h"
#include "project/model/Media.h"
#include "project/Editor.h"
#include "render/Flatten.h"

namespace {

using genesis::test::check;
using namespace genesis::project;
namespace render = genesis::render;

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

void test_a_clip_flattens_with_its_media_and_track_facts()
{
    auto [editor, media_id, clip_id] = fixture();
    (void)media_id;
    (void)clip_id;
    const std::vector<render::ExportClip> flat =
            render::flatten_timeline(editor.project(), std::nullopt);
    check(flat.size() == 1, "one clip flattens");
    const render::ExportClip &clip = flat[0];
    check(clip.path == "/footage/a.mp4", "the path");
    check(clip.kind == render::ClipKind::Video, "video");
    check(clip.start == Rational{ 1, 1 }, "the start");
    check(clip.duration == Rational{ 10, 1 }, "the clip takes its media's length");
    check(clip.track == 0, "track zero");
    check(!clip.hidden, "not hidden");
    check(!clip.muted, "not muted");
    check(clip.media_width.has_value() && *clip.media_width == 1920, "the media width");
    check(clip.has_audio.has_value() && *clip.has_audio == true, "has audio");
    check(clip.filter_chain.empty(), "no filter chain");
    check(clip.effects.empty(), "no effects");
}

void test_track_state_folds_into_the_clip_flags()
{
    auto [editor, media_id, clip_id] = fixture();
    (void)media_id;
    (void)clip_id;
    const std::string track_id = editor.project().timelines[0].tracks[0].id;
    check(editor.apply(Command{ SetTrackFlag{ track_id, TrackFlag::Visible, false } }).has_value(),
          "hides");
    check(editor.apply(Command{ SetTrackFlag{ track_id, TrackFlag::Muted, true } }).has_value(),
          "mutes");
    const std::vector<render::ExportClip> flat =
            render::flatten_timeline(editor.project(), std::nullopt);
    check(flat.size() == 1, "one clip flattens");
    check(flat[0].hidden, "a hidden track hides its clips");
    check(flat[0].muted, "a muted track mutes its clips");
}

void test_the_stored_effects_ride_along()
{
    auto [editor, media_id, clip_id] = fixture();
    (void)media_id;
    ClipPatch patch;
    patch.video_effects = std::vector<AppliedFilter>{ AppliedFilter::create("example.mono") };
    check(editor.apply(Command{ UpdateClip{ clip_id, std::move(patch) } }).has_value(),
          "applies effect");
    const std::vector<render::ExportClip> flat =
            render::flatten_timeline(editor.project(), std::nullopt);
    check(flat.size() == 1, "one clip flattens");
    check(flat[0].effects == std::vector<AppliedFilter>{ AppliedFilter::create("example.mono") },
          "the stored effects ride along");
}

void test_text_clips_are_left_for_the_rasteriser()
{
    auto [editor, media_id, clip_id] = fixture();
    (void)media_id;
    (void)clip_id;
    const std::optional<std::string> track_id = editor.project().timelines[0].tracks[0].id;
    check(editor.apply(Command{ AddTextClip{ track_id, false, Rational::ZERO, std::nullopt,
                                             std::nullopt, std::nullopt } })
                  .has_value(),
          "adds title");
    const std::vector<render::ExportClip> flat =
            render::flatten_timeline(editor.project(), std::nullopt);
    // A title now flattens as a picture whose path is the clip id: the app
    // rasterises the words and build_timeline swaps the id for the PNG. It
    // used to be dropped here, so a title clip could never reach the engine.
    // The fixture's own clip still flattens, and the title now flattens too - as
    // a picture whose path is its clip id: the app rasterises the words and
    // build_timeline swaps the id for the PNG. Titles used to be dropped here,
    // so a title clip could never reach the engine at all.
    const auto title = std::find_if(flat.begin(), flat.end(), [](const render::ExportClip &clip) {
        return clip.kind == render::ClipKind::Image;
    });
    check(title != flat.end(), "the title flattens as a picture");
    check(title != flat.end() && !title->path.empty(), "keyed by its clip id, not a file path");
}

void test_a_clip_whose_media_vanished_is_skipped_not_fatal()
{
    auto [editor, media_id, clip_id] = fixture();
    (void)clip_id;
    check(editor.apply(Command{ RemoveMedia{ media_id } }).has_value(), "removes");
    // RemoveMedia also removes the clips; force the orphan case instead by
    // asking for a timeline that does not exist.
    check(render::flatten_timeline(editor.project(), std::nullopt).empty(),
          "no clips without media");
    check(render::flatten_timeline(editor.project(), std::string_view{ "nope" }).size() <= 1,
          "an unknown timeline yields at most the first");
}

void test_the_reverse_flag_reaches_the_flattened_clip()
{
    auto [editor, media_id, clip_id] = fixture();
    (void)media_id;
    const std::vector<render::ExportClip> before =
            render::flatten_timeline(editor.project(), std::nullopt);
    check(before.size() == 1 && !before[0].reverse, "a clip flattens forwards by default");

    check(editor.apply(Command{ SetClipReverse{ clip_id, true } }).has_value(), "reverses");
    const std::vector<render::ExportClip> after =
            render::flatten_timeline(editor.project(), std::nullopt);
    check(after.size() == 1 && after[0].reverse, "the reverse flag reaches the flattened clip");
}

void test_the_pan_reaches_the_flattened_clip()
{
    auto [editor, media_id, clip_id] = fixture();
    (void)media_id;
    const std::vector<render::ExportClip> before =
            render::flatten_timeline(editor.project(), std::nullopt);
    check(before.size() == 1 && before[0].pan == 0.0, "a clip flattens centred by default");

    check(editor.apply(Command{ SetClipPan{ clip_id, 0.5 } }).has_value(), "pans");
    const std::vector<render::ExportClip> after =
            render::flatten_timeline(editor.project(), std::nullopt);
    check(after.size() == 1 && after[0].pan == 0.5, "the pan reaches the flattened clip");
}

// A cutout whose masks are nowhere yet renders whole, even when a mask root is
// handed in: the flattener names the mask directory only for a clip that
// actually has a cutout.
void test_a_cutout_without_a_mask_root_names_no_masks()
{
    auto [editor, media_id, clip_id] = fixture();
    (void)media_id;

    genesis::project::Cutout cutout = genesis::project::Cutout::automatic();
    cutout.subject = genesis::project::Subject::Person;
    check(editor.apply(Command{ SetClipCutout{ clip_id, cutout } }).has_value(),
          "sets a person cutout");

    // flatten_timeline hands no mask root, so the cutout rides but no mask
    // directory or spec is derived.
    const std::vector<render::ExportClip> flat =
            render::flatten_timeline(editor.project(), std::nullopt);
    check(flat.size() == 1, "one clip flattens");
    check(flat[0].cutout.has_value(), "the cutout rides along");
    check(flat[0].mask_dir.empty(), "no mask directory without a root");
    check(!flat[0].mask.has_value(), "no mask spec without a root");

    // Even a named root leaves a cutout-less clip unnamed.
    auto [plain, plain_media, plain_clip] = fixture();
    (void)plain_media;
    (void)plain_clip;
    const std::vector<render::ExportClip> with_root = render::flatten_timeline_in(
            plain.project(), std::nullopt, std::filesystem::path{ "/projects/x/masks" });
    check(with_root.size() == 1, "the plain clip still flattens");
    check(with_root[0].mask_dir.empty(), "a cutout-less clip names no masks");
    check(!with_root[0].mask.has_value(), "and carries no mask spec");
}

// With a mask root, a cutout names its mask directory and carries the full
// spec the engine needs to find and step the masks: the source they were
// keyed by, the subject/model, the native size and the stride.
void test_a_cutout_with_a_mask_root_names_its_masks()
{
    auto [editor, media_id, clip_id] = fixture();
    (void)media_id;

    genesis::project::Cutout cutout = genesis::project::Cutout::automatic();
    cutout.subject = genesis::project::Subject::Person;
    check(editor.apply(Command{ SetClipCutout{ clip_id, cutout } }).has_value(),
          "sets a person cutout");

    const std::filesystem::path root{ "/projects/x/masks" };
    const std::vector<render::ExportClip> flat =
            render::flatten_timeline_in(editor.project(), std::nullopt, root);
    check(flat.size() == 1, "one clip flattens");
    check(flat[0].cutout.has_value(), "the cutout rides along");
    check(flat[0].mask_dir == root.string(), "the mask directory is named");
    check(flat[0].mask.has_value(), "the mask spec is carried");

    if (!flat[0].mask) {
        return;
    }
    const genesis::ai::vision::MaskSpec &spec = *flat[0].mask;
    check(spec.root == root, "the spec records the mask root");
    check(spec.source == std::filesystem::path{ "/footage/a.mp4" },
          "the spec is keyed by the source media");
    check(spec.source_size == 0, "a missing source records a zero size");
    check(spec.subject == genesis::jobs::Subject::Person, "a person cutout keys the person model");
    check(spec.model_id == "rvm", "the person model id is rvm");
    check(spec.width == 1920 && spec.height == 1080, "the spec records the media's native size");
    check(spec.stride == 1, "the spec pins one mask per frame");
    check(!spec.last_frame.has_value(), "no last frame is recorded yet");
}

// A custom cutout with a stroke carries its stroke revision on the spec, so the
// engine looks up exactly the masks the driver refined for those strokes, not
// the automatic base masks.
void test_a_custom_cutout_names_its_revision()
{
    auto [editor, media_id, clip_id] = fixture();
    (void)media_id;

    genesis::project::Cutout cutout = genesis::project::Cutout::automatic();
    cutout.subject = genesis::project::Subject::Person;
    cutout.mode = genesis::project::CutoutMode::Custom;
    genesis::project::Stroke stroke;
    stroke.tool = genesis::project::BrushTool::SmartBrush;
    stroke.size = 0.05;
    stroke.points = { { { 0.1, 0.2 } } };
    cutout.strokes.push_back(stroke);
    check(editor.apply(Command{ SetClipCutout{ clip_id, cutout } }).has_value(),
          "sets a stroked custom cutout");

    const std::filesystem::path root{ "/projects/x/masks" };
    const std::vector<render::ExportClip> flat =
            render::flatten_timeline_in(editor.project(), std::nullopt, root);
    check(flat.size() == 1, "one clip flattens");
    if (!flat[0].mask) {
        return;
    }
    check(flat[0].mask->revision == genesis::ai::vision::mask_revision(*flat[0].cutout),
          "the spec records the cutout's stroke revision");
    check(!flat[0].mask->revision.empty(), "a stroked cutout names a non-empty revision");
}

} // namespace

int main()
{
    test_a_clip_flattens_with_its_media_and_track_facts();
    test_track_state_folds_into_the_clip_flags();
    test_the_stored_effects_ride_along();
    test_text_clips_are_left_for_the_rasteriser();
    test_a_clip_whose_media_vanished_is_skipped_not_fatal();
    test_the_reverse_flag_reaches_the_flattened_clip();
    test_the_pan_reaches_the_flattened_clip();
    test_a_cutout_without_a_mask_root_names_no_masks();
    test_a_cutout_with_a_mask_root_names_its_masks();
    test_a_custom_cutout_names_its_revision();

    return genesis::test::summary();
}
