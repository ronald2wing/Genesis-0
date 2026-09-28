// SPDX-License-Identifier: GPL-3.0-or-later
// SPDX-FileCopyrightText: 2026 Genesis-0 contributors

#include <cstdio>
#include <optional>
#include <span>
#include <string>
#include <utility>
#include <vector>

#include "../support/Checks.h"
#include "project/command/ClipCommands.h"
#include "project/command/Command.h"
#include "project/command/MediaCommands.h"
#include "project/model/Clip.h"
#include "project/model/Media.h"
#include "project/Editor.h"
#include "render/Flatten.h"
#include "render/Resolve.h"

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

Fixture fixture(bool has_audio)
{
    Fixture f;
    NewMedia item;
    item.path = "/footage/a.mp4";
    item.name = "a.mp4";
    item.duration = Rational{ 10, 1 };
    item.kind = MediaKind::Video;
    item.width = 1920;
    item.height = 1080;
    item.has_audio = has_audio;
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

void test_clips_resolve_with_their_exact_spans()
{
    auto [editor, media_id, clip_id] = fixture(false);
    (void)clip_id;
    // A second clip of the same media, on the next lane, starting at 20s.
    const std::string track1 = editor.project().timelines[0].tracks[1].id;
    check(editor.apply(Command{ AddClip{ media_id, track1, Rational{ 20, 1 }, false } })
                  .has_value(),
          "adds a second clip");

    const std::vector<render::ExportClip> flat =
            render::flatten_timeline(editor.project(), std::nullopt);
    check(flat.size() == 2, "two clips flatten");

    render::ExportRequest request;
    request.width = 1920;
    request.height = 1080;
    const render::BuiltTimeline built =
            render::build_timeline(request, genesis::core::FrameRate::THIRTY,
                                   std::span<const render::ExportClip>(flat), { });

    check(built.timeline.clip_count() == 2, "two clips resolve");
    check(built.timeline.track_ids().size() == 2, "lanes cover the highest occupied track");

    // The first clip sits on the bottom lane at 1s, ten seconds long.
    const std::optional<genesis::core::ClipId> first =
            built.timeline.clip_on_track_at(built.timeline.track_ids()[0], Rational{ 2, 1 });
    check(first.has_value(), "the first clip is on its lane");
    const genesis::core::Clip *first_clip = built.timeline.clip(*first);
    check(first_clip != nullptr && first_clip->start() == Rational{ 1, 1 }
                  && first_clip->duration() == Rational{ 10, 1 },
          "the first clip's span is exact");

    // The second sits on the next lane at 20s.
    const std::optional<genesis::core::ClipId> second =
            built.timeline.clip_on_track_at(built.timeline.track_ids()[1], Rational{ 21, 1 });
    check(second.has_value(), "the second clip is on its lane");
    const genesis::core::Clip *second_clip = built.timeline.clip(*second);
    check(second_clip != nullptr && second_clip->start() == Rational{ 20, 1 }
                  && second_clip->duration() == Rational{ 10, 1 },
          "the second clip's span is exact");
}

void test_audio_bearing_clip_resolves_onto_an_audio_track()
{
    auto [editor, media_id, clip_id] = fixture(true);
    (void)media_id;
    (void)clip_id;

    const std::vector<render::ExportClip> flat =
            render::flatten_timeline(editor.project(), std::nullopt);
    check(flat.size() == 1, "one clip flattens");

    render::ExportRequest request;
    request.width = 1920;
    request.height = 1080;
    const render::BuiltTimeline built =
            render::build_timeline(request, genesis::core::FrameRate::THIRTY,
                                   std::span<const render::ExportClip>(flat), { });

    check(built.timeline.clip_count() == 1, "one clip resolves");
    // The clip's lane plus the audio track created for its embedded sound.
    check(built.timeline.track_ids().size() == 2, "a video lane and its audio track");

    // The clip stays on its video lane; its sound is signalled beside it.
    const std::optional<genesis::core::ClipId> id =
            built.timeline.clip_on_track_at(built.timeline.track_ids()[0], Rational{ 2, 1 });
    check(id.has_value(), "the clip is on its video lane");
    if (!id.has_value()) {
        return;
    }
    const auto audio = built.audio.find(*id);
    check(audio != built.audio.end(), "the clip carries audio facts");
    if (audio != built.audio.end()) {
        check(audio->second.volume == 1.0, "unity gain by default");
        check(audio->second.pan == 0.0, "centred pan by default");
        check(audio->second.fade_in == Rational::ZERO, "no fade in");
        check(audio->second.fade_out == Rational::ZERO, "no fade out");
    }
}

void test_quantise_pins_the_frame_grid()
{
    // A third of a second at 29.97 fps is ten frames: 1001/3000.
    check(render::quantise(1.0 / 3.0, genesis::core::FrameRate::NTSC_30) == Rational{ 1001, 3000 },
          "quantise rounds to the nearest frame");
    // A whole frame passes through exactly.
    check(render::quantise(0.5, genesis::core::FrameRate::THIRTY) == Rational{ 1, 2 },
          "quantise of a whole frame is exact");
}

} // namespace

int main()
{
    test_clips_resolve_with_their_exact_spans();
    test_audio_bearing_clip_resolves_onto_an_audio_track();
    test_quantise_pins_the_frame_grid();

    return genesis::test::summary();
}
