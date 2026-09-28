// SPDX-License-Identifier: GPL-3.0-or-later
// SPDX-FileCopyrightText: 2026 Genesis-0 contributors
//
// Ported from Concat's concat-project inline tests (lib.rs),
// SPDX-License-Identifier: GPL-3.0-or-later,
// SPDX-FileCopyrightText: 2026 Jareer and Concat contributors.

#include <cmath>
#include <cstdint>
#include <cstdio>
#include <limits>
#include <optional>
#include <string>
#include <vector>

#include "core/SpeedCurve.h"
#include "project/Project.h"
#include "project/command/AudioCommands.h"
#include "project/command/ClipCommands.h"
#include "project/command/Command.h"
#include "project/command/Error.h"
#include "project/command/MediaCommands.h"
#include "project/command/PropertyCommands.h"
#include "project/command/TimelineCommands.h"
#include "project/command/TrackCommands.h"
#include "project/model/Clip.h"
#include "project/model/Effects.h"
#include "project/model/Keyframe.h"
#include "project/model/Media.h"
#include "project/model/Speed.h"
#include "project/model/Text.h"
#include "project/model/Timeline.h"
#include "project/model/Track.h"
#include "project/Editor.h"

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

void check_near(double got, double want, double tolerance, const std::string &what)
{
    check(std::abs(got - want) <= tolerance,
          what + " (got " + std::to_string(got) + ", want " + std::to_string(want) + ")");
}

using namespace genesis::project;

// --- test helpers -----------------------------------------------------------

Command media(const std::string &path, Rational duration, bool has_audio)
{
    NewMedia item;
    item.path = path;
    item.name = path;
    const std::size_t slash = path.find_last_of('/');
    if (slash != std::string::npos) {
        item.name = path.substr(slash + 1);
    }
    item.duration = duration;
    item.kind = MediaKind::Video;
    item.width = 1920;
    item.height = 1080;
    item.frame_rate = 30.0;
    item.frame_rate_fraction = "30/1";
    item.video_codec = "h264";
    if (has_audio) {
        item.audio_codec = "aac";
    }
    item.has_audio = has_audio;
    return Command{ AddMedia{ std::move(item) } };
}

// A recording that kept its desktop sound and microphone apart: two audio
// streams, the second named.
Command two_track_media(const std::string &path)
{
    NewMedia item;
    item.path = path;
    item.name = path;
    const std::size_t slash = path.find_last_of('/');
    if (slash != std::string::npos) {
        item.name = path.substr(slash + 1);
    }
    item.duration = Rational{ 10, 1 };
    item.kind = MediaKind::Video;
    item.width = 1920;
    item.height = 1080;
    item.frame_rate = 30.0;
    item.frame_rate_fraction = "30/1";
    item.video_codec = "h264";
    item.audio_codec = "aac";
    item.has_audio = true;
    item.audio_tracks.push_back(AudioTrack{ 1, "aac", 2, 48000, "", "" });
    item.audio_tracks.push_back(AudioTrack{ 2, "aac", 1, 48000, "Mic/Aux", "" });
    return Command{ AddMedia{ std::move(item) } };
}

// Editor with one media item and one clip at [0, 10) on track one.
struct Fixture
{
    Editor editor;
    std::string media_id;
    std::string clip_id;
};

Fixture fixture()
{
    Fixture f;
    const auto add_media = f.editor.apply(media("/a.mp4", Rational{ 10, 1 }, true));
    check(add_media.has_value(), "adds");
    check(add_media->created_id.has_value(), "id");
    f.media_id = *add_media->created_id;
    const std::string track_id = f.editor.project().active().tracks[0].id;
    const auto add_clip = f.editor.apply(AddClip{ f.media_id, track_id, Rational::ZERO, false });
    check(add_clip.has_value(), "adds");
    check(add_clip->created_id.has_value(), "id");
    f.clip_id = *add_clip->created_id;
    return f;
}

// Clips of `media_id`, ten seconds each, at these starts on this track, in
// this order. The ids come back in the same order.
std::vector<std::string> lane(Editor &editor, const std::string &media_id,
                              const std::string &track_id, const std::vector<Rational> &starts)
{
    std::vector<std::string> ids;
    ids.reserve(starts.size());
    for (const Rational &start : starts) {
        const auto outcome = editor.apply(AddClip{ media_id, track_id, start, false });
        check(outcome.has_value(), "adds");
        check(outcome->created_id.has_value(), "id");
        ids.push_back(*outcome->created_id);
    }
    return ids;
}

Rational start_of(const Editor &editor, const std::string &clip_id)
{
    const Clip *clip = editor.project().active().clip(clip_id);
    check(clip != nullptr, "the clip is still there");
    return clip != nullptr ? clip->start : Rational::ZERO;
}

// --- ripple delete: https://github.com/jub0t/Concat/issues/106 ---------------

void test_ripple_delete_closes_the_gap_on_its_own_track_only()
{
    auto [editor, media_id, first] = fixture();
    const auto &tracks = editor.project().active().tracks;
    const std::string video = tracks[0].id;
    const std::string sound = tracks[1].id;
    const auto rest = lane(editor, media_id, video, { Rational{ 10, 1 }, Rational{ 20, 1 } });
    const auto other = lane(editor, media_id, sound, { Rational{ 15, 1 } });

    const auto outcome = editor.apply(RemoveClips{ std::vector<std::string>{ rest[0] }, true });
    check(outcome.has_value(), "removes");
    check(outcome->applied, "applied");
    check(editor.project().active().clip(rest[0]) == nullptr, "the removed clip is gone");
    check(start_of(editor, first) == Rational::ZERO, "what was in front stays");
    check(start_of(editor, rest[1]) == Rational{ 10, 1 }, "what was behind closes up");
    check(start_of(editor, other[0]) == Rational{ 15, 1 },
          "another lane is none of this deletion's business");
}

void test_a_plain_delete_still_leaves_the_hole()
{
    auto [editor, media_id, first] = fixture();
    (void)first;
    const std::string video = editor.project().active().tracks[0].id;
    const auto rest = lane(editor, media_id, video, { Rational{ 10, 1 }, Rational{ 20, 1 } });
    const auto outcome = editor.apply(RemoveClips{ std::vector<std::string>{ rest[0] }, false });
    check(outcome.has_value(), "removes");
    check(start_of(editor, rest[1]) == Rational{ 20, 1 }, "the hole stays");
}

void test_ripple_delete_of_several_clips_moves_each_survivor_by_what_was_before_it()
{
    auto [editor, media_id, first] = fixture();
    const std::string video = editor.project().active().tracks[0].id;
    const auto rest = lane(editor, media_id, video,
                           { Rational{ 10, 1 }, Rational{ 20, 1 }, Rational{ 30, 1 } });
    // The first and the third go: the second moves by one span, the fourth by
    // two.
    const auto outcome =
            editor.apply(RemoveClips{ std::vector<std::string>{ first, rest[1] }, true });
    check(outcome.has_value(), "removes");
    check(start_of(editor, rest[0]) == Rational::ZERO, "the second moves up by one span");
    check(start_of(editor, rest[2]) == Rational{ 10, 1 }, "the fourth moves up by two spans");
    check(editor.project().active().clips.size() == 2, "two clips remain");
}

void test_ripple_delete_counts_overlapping_spans_once()
{
    auto [editor, media_id, first] = fixture();
    const std::string video = editor.project().active().tracks[0].id;
    // Two doomed clips sharing five seconds - [0, 10) and [5, 15) - and a
    // survivor at twenty. Their union is fifteen, so it lands at five; their
    // sum is twenty, which would send it to zero. Away from zero on purpose:
    // the floor there would hide the difference.
    const auto more = lane(editor, media_id, video, { Rational{ 5, 1 }, Rational{ 20, 1 } });
    const auto outcome =
            editor.apply(RemoveClips{ std::vector<std::string>{ first, more[0] }, true });
    check(outcome.has_value(), "removes");
    check_near(start_of(editor, more[1]).as_double(), 5.0, 1e-9,
               "the union of the two spans, not their sum");
}

void test_ripple_delete_of_a_span_reaching_past_a_survivor_pulls_only_up_to_it()
{
    auto [editor, media_id, first] = fixture();
    const std::string video = editor.project().active().tracks[0].id;
    // A doomed clip at [10, 20) with a survivor stacked inside its span at
    // fourteen: the survivor moves by the four seconds of span in front of it,
    // to ten, not by the whole ten, which would put it at four - in front of
    // where the doomed clip began.
    const auto more = lane(editor, media_id, video, { Rational{ 10, 1 }, Rational{ 14, 1 } });
    const auto outcome = editor.apply(RemoveClips{ std::vector<std::string>{ more[0] }, true });
    check(outcome.has_value(), "removes");
    check(start_of(editor, first) == Rational::ZERO, "what was in front stays");
    check(start_of(editor, more[1]) == Rational{ 10, 1 }, "pulled only up to the survivor");
}

void test_ripple_delete_leaves_a_clip_stacked_at_the_same_start_alone()
{
    auto [editor, media_id, first] = fixture();
    const std::string video = editor.project().active().tracks[0].id;
    const auto twin = lane(editor, media_id, video, { Rational::ZERO });
    const auto outcome = editor.apply(RemoveClips{ std::vector<std::string>{ first }, true });
    check(outcome.has_value(), "removes");
    check(start_of(editor, twin[0]) == Rational::ZERO, "nothing was in front of it");
}

void test_ripple_delete_of_unknown_ids_moves_nothing_and_records_nothing()
{
    auto [editor, media_id, first] = fixture();
    (void)first;
    const std::string video = editor.project().active().tracks[0].id;
    const auto rest = lane(editor, media_id, video, { Rational{ 10, 1 } });
    const bool before_can_undo = editor.can_undo();
    const auto outcome = editor.apply(RemoveClips{ std::vector<std::string>{ "nobody" }, true });
    check(outcome.has_value(), "tolerated");
    check(!outcome->applied, "a no-op changes nothing");
    check(start_of(editor, rest[0]) == Rational{ 10, 1 }, "the survivor stays");
    check(editor.can_undo() == before_can_undo, "a no-op is not a step");
}

void test_ripple_delete_is_one_undo_step_that_puts_everything_back()
{
    auto [editor, media_id, first] = fixture();
    const std::string video = editor.project().active().tracks[0].id;
    const auto rest = lane(editor, media_id, video, { Rational{ 10, 1 }, Rational{ 20, 1 } });
    const auto outcome =
            editor.apply(RemoveClips{ std::vector<std::string>{ first, rest[0] }, true });
    check(outcome.has_value(), "removes");
    check(start_of(editor, rest[1]) == Rational::ZERO, "the lane closes");
    editor.undo();
    check(start_of(editor, first) == Rational::ZERO, "the first is back");
    check(start_of(editor, rest[0]) == Rational{ 10, 1 }, "the second is back");
    check(start_of(editor, rest[1]) == Rational{ 20, 1 }, "the third is back");
    editor.redo();
    check(start_of(editor, rest[1]) == Rational::ZERO, "redo closes the lane again");
}

void test_ripple_delete_of_everything_on_a_lane_is_just_a_delete()
{
    auto [editor, media_id, first] = fixture();
    const std::string video = editor.project().active().tracks[0].id;
    const auto rest = lane(editor, media_id, video, { Rational{ 10, 1 } });
    const auto outcome =
            editor.apply(RemoveClips{ std::vector<std::string>{ first, rest[0] }, true });
    check(outcome.has_value(), "removes");
    check(editor.project().active().clips.empty(), "the lane is empty");
}

// --- magnetic trim: https://github.com/jub0t/Concat/issues/106 ---------------

Command trim(const std::string &clip_id, TrimEdge edge, Rational delta, bool ripple)
{
    return Command{ TrimClip{ clip_id, edge, delta, ripple } };
}

void test_magnetic_tail_trim_moves_the_lane_behind_it_both_ways()
{
    auto [editor, media_id, first] = fixture();
    const auto &tracks = editor.project().active().tracks;
    const std::string video = tracks[0].id;
    const std::string sound = tracks[1].id;
    const auto behind = lane(editor, media_id, video, { Rational{ 10, 1 }, Rational{ 20, 1 } });
    const auto other = lane(editor, media_id, sound, { Rational{ 12, 1 } });

    const auto shorten = editor.apply(trim(first, TrimEdge::End, Rational{ -2, 1 }, true));
    check(shorten.has_value(), "shortens");
    check(start_of(editor, behind[0]) == Rational{ 8, 1 }, "closes up");
    check(start_of(editor, behind[1]) == Rational{ 18, 1 }, "and the one behind that");
    check(start_of(editor, other[0]) == Rational{ 12, 1 }, "another lane stays");

    const auto lengthen = editor.apply(trim(first, TrimEdge::End, Rational{ 3, 1 }, true));
    check(lengthen.has_value(), "lengthens");
    check(start_of(editor, behind[0]) == Rational{ 11, 1 }, "makes room");
    check(start_of(editor, behind[1]) == Rational{ 21, 1 }, "and the one behind that");
}

void test_a_plain_trim_still_leaves_the_lane_alone()
{
    auto [editor, media_id, first] = fixture();
    const std::string video = editor.project().active().tracks[0].id;
    const auto behind = lane(editor, media_id, video, { Rational{ 10, 1 } });
    const auto outcome = editor.apply(trim(first, TrimEdge::End, Rational{ -2, 1 }, false));
    check(outcome.has_value(), "shortens");
    check(start_of(editor, behind[0]) == Rational{ 10, 1 }, "the lane stays");
}

void test_magnetic_head_trim_keeps_the_clip_where_it_was_and_closes_behind_it()
{
    auto [editor, media_id, first] = fixture();
    const std::string video = editor.project().active().tracks[0].id;
    const auto behind = lane(editor, media_id, video, { Rational{ 10, 1 } });
    const auto outcome = editor.apply(trim(first, TrimEdge::Start, Rational{ 2, 1 }, true));
    check(outcome.has_value(), "trims the head");
    const Clip *clip = editor.project().active().clip(first);
    check(clip != nullptr, "kept");
    check(clip->start == Rational::ZERO, "the clip does not move");
    check(clip->duration == Rational{ 8, 1 }, "the duration shortens");
    check(clip->source_start == Rational{ 2, 1 }, "the in-point does");
    check(start_of(editor, behind[0]) == Rational{ 8, 1 }, "and the lane closes");
}

void test_a_plain_head_trim_moves_the_head_and_nothing_else()
{
    auto [editor, media_id, first] = fixture();
    const std::string video = editor.project().active().tracks[0].id;
    const auto behind = lane(editor, media_id, video, { Rational{ 10, 1 } });
    const auto outcome = editor.apply(trim(first, TrimEdge::Start, Rational{ 2, 1 }, false));
    check(outcome.has_value(), "trims the head");
    check(start_of(editor, first) == Rational{ 2, 1 }, "the head moves");
    check(start_of(editor, behind[0]) == Rational{ 10, 1 }, "nothing else does");
}

void test_magnetic_head_trim_can_reach_back_only_as_far_as_the_source_and_moves_nothing_when_it_cannot()
{
    auto [editor, media_id, first] = fixture();
    const std::string video = editor.project().active().tracks[0].id;
    const auto behind = lane(editor, media_id, video, { Rational{ 10, 1 } });
    const bool before_can_undo = editor.can_undo();
    // In-point at zero: there is nothing before it to restore.
    const auto outcome = editor.apply(trim(first, TrimEdge::Start, Rational{ -3, 1 }, true));
    check(outcome.has_value(), "tolerated");
    check(!outcome->applied, "a no-op changes nothing");
    check(start_of(editor, behind[0]) == Rational{ 10, 1 }, "the lane stays");
    check(editor.can_undo() == before_can_undo, "a no-op is not a step");
}

void test_magnetic_head_trim_restoring_source_pushes_the_lane_back()
{
    auto [editor, media_id, first] = fixture();
    const std::string video = editor.project().active().tracks[0].id;
    const auto behind = lane(editor, media_id, video, { Rational{ 10, 1 } });
    const auto cut = editor.apply(trim(first, TrimEdge::Start, Rational{ 4, 1 }, true));
    check(cut.has_value(), "cuts four off the head");
    check(start_of(editor, behind[0]) == Rational{ 6, 1 }, "the lane closes by four");
    // Ask for five back: only four exist, so four come back.
    const auto restore = editor.apply(trim(first, TrimEdge::Start, Rational{ -5, 1 }, true));
    check(restore.has_value(), "restores what there is");
    const Clip *clip = editor.project().active().clip(first);
    check(clip != nullptr, "kept");
    check(clip->start == Rational::ZERO, "a magnetic clip at zero still does not move");
    check(clip->source_start == Rational::ZERO, "the in-point is back to zero");
    check(clip->duration == Rational{ 10, 1 }, "the duration is back");
    check(start_of(editor, behind[0]) == Rational{ 10, 1 }, "the lane went back by four");
}

void test_magnetic_trim_stops_at_the_minimum_and_the_lane_stays_touching()
{
    auto [editor, media_id, first] = fixture();
    const std::string video = editor.project().active().tracks[0].id;
    const auto behind = lane(editor, media_id, video, { Rational{ 10, 1 } });
    const auto outcome = editor.apply(trim(first, TrimEdge::End, Rational{ -100, 1 }, true));
    check(outcome.has_value(), "clamps");
    const Clip *clip = editor.project().active().clip(first);
    check(clip != nullptr, "kept");
    check(clip->duration > Rational::ZERO && clip->duration < Rational::ONE,
          "floored at the minimum");
    const Rational gap = start_of(editor, behind[0]) - (clip->start + clip->duration);
    check(std::abs(gap.as_double()) < 1e-9,
          "the lane moved by the clamped amount, not the asked one");
}

void test_magnetic_trim_leaves_a_clip_in_front_of_the_edge_alone()
{
    auto [editor, media_id, first] = fixture();
    const std::string video = editor.project().active().tracks[0].id;
    // A clip behind the trimmed one, and one stacked before its tail on
    // the same lane: a tail trim is about what is behind the tail.
    const auto more = lane(editor, media_id, video, { Rational{ 10, 1 }, Rational{ 3, 1 } });
    const auto outcome = editor.apply(trim(first, TrimEdge::End, Rational{ -2, 1 }, true));
    check(outcome.has_value(), "shortens");
    check(start_of(editor, more[0]) == Rational{ 8, 1 }, "behind closes up");
    check(start_of(editor, more[1]) == Rational{ 3, 1 }, "in front of the edge; not moved");
}

void test_magnetic_trim_is_one_undo_step_that_puts_the_lane_back()
{
    auto [editor, media_id, first] = fixture();
    const std::string video = editor.project().active().tracks[0].id;
    const auto behind = lane(editor, media_id, video, { Rational{ 10, 1 } });
    const auto outcome = editor.apply(trim(first, TrimEdge::Start, Rational{ 2, 1 }, true));
    check(outcome.has_value(), "trims");
    check(start_of(editor, behind[0]) == Rational{ 8, 1 }, "the lane closes");
    editor.undo();
    check(start_of(editor, behind[0]) == Rational{ 10, 1 }, "undo puts it back");
    const Clip *clip = editor.project().active().clip(first);
    check(clip != nullptr, "kept");
    check(clip->start == Rational::ZERO && clip->duration == Rational{ 10, 1 }
                  && clip->source_start == Rational::ZERO,
          "the clip is back as it was");
}

// --- replace clip media ------------------------------------------------------

void test_replace_clip_media_can_move_the_in_point_to_the_copy()
{
    auto [editor, media_id, clip_id] = fixture();
    (void)media_id;
    const auto trimmed = editor.apply(trim(clip_id, TrimEdge::Start, Rational{ 2, 1 }, false));
    check(trimmed.has_value(), "trims");
    const Clip *before = editor.project().active().clip(clip_id);
    check(before != nullptr, "clip");
    check(before->source_start == Rational{ 2, 1 } && before->duration == Rational{ 8, 1 },
          "the head trim moved the in-point and shortened the clip");

    NewMedia copy;
    copy.path = "/project/cache/reverse-1-2000-8000.mp4";
    copy.name = "a.mp4 (reversed)";
    copy.duration = Rational{ 8, 1 };
    copy.kind = MediaKind::Video;
    copy.width = 1920;
    copy.height = 1080;
    copy.frame_rate = 30.0;
    copy.frame_rate_fraction = "30/1";
    copy.video_codec = "h264";
    copy.has_audio = false;

    const auto replaced =
            editor.apply(ReplaceClipMedia{ clip_id, std::move(copy), Rational{ 0, 1 } });
    check(replaced.has_value(), "replaces");
    check(replaced->applied, "applied");
    const Clip *clip = editor.project().active().clip(clip_id);
    check(clip != nullptr, "clip");
    check(clip->source_start == Rational::ZERO, "the copy starts at its own zero");
    check(clip->duration == Rational{ 8, 1 }, "the length is kept");
}

void test_replace_clip_media_points_the_clip_at_the_copy_and_keeps_the_original()
{
    auto [editor, media_id, clip_id] = fixture();

    NewMedia copy;
    copy.path = "/project/cache/enhance-1-2x.mp4";
    copy.name = "a.mp4 (enhanced)";
    copy.duration = Rational{ 10, 1 };
    copy.kind = MediaKind::Video;
    copy.width = 3840;
    copy.height = 2160;
    copy.frame_rate = 30.0;
    copy.frame_rate_fraction = "30/1";
    copy.video_codec = "h264";
    copy.audio_codec = "aac";
    copy.has_audio = true;

    const auto replaced = editor.apply(ReplaceClipMedia{ clip_id, copy, std::nullopt });
    check(replaced.has_value(), "replaces");
    check(replaced->applied, "applied");
    check(replaced->created_id.has_value(), "the copy's id");
    const std::string copy_id = replaced->created_id.value_or("");
    check(copy_id != media_id, "a fresh id, not the original's");
    const Clip *clip = editor.project().active().clip(clip_id);
    check(clip != nullptr, "clip");
    check(clip->media_id == copy_id, "the clip now points at the copy");
    check(clip->source_start == Rational::ZERO, "the in-point is kept");
    check(editor.project().media.size() == 2, "the original stays in the bin");
    const MediaItem *copy_item = editor.project().media_by_id(copy_id);
    check(copy_item != nullptr, "the copy is in the bin");
    check(copy_item->width.has_value() && *copy_item->width == 3840,
          "the copy keeps its own width");

    // The same copy again changes nothing and mints nothing.
    const auto again = editor.apply(ReplaceClipMedia{ clip_id, copy, std::nullopt });
    check(again.has_value(), "no-op");
    check(!again->applied, "a no-op changes nothing");
    check(editor.project().media.size() == 2, "still two items");

    // A clip nobody has is a no-op that adds nothing to the bin.
    NewMedia elsewhere = copy;
    elsewhere.path = "/elsewhere.mp4";
    const auto nobody =
            editor.apply(ReplaceClipMedia{ "c999", std::move(elsewhere), std::nullopt });
    check(nobody.has_value(), "no-op");
    check(!nobody->applied, "a missing clip is a no-op");
    check(editor.project().media.size() == 2, "nothing was added to the bin");

    // A non-finite number in the copy's facts is refused, like an import's.
    NewMedia bad;
    bad.path = "/nan.mp4";
    bad.kind = MediaKind::Video;
    bad.frame_rate = std::nan("");
    bad.has_audio = false;
    const auto refused = editor.apply(ReplaceClipMedia{ clip_id, std::move(bad), std::nullopt });
    check(!refused.has_value(), "a non-finite fact is refused");
    check(refused.error().kind == CommandErrorKind::NotANumber, "refused as not a number");

    // Undo puts the clip back on the original; the copy stays in the bin,
    // harmless, for redo.
    editor.undo();
    const Clip *restored = editor.project().active().clip(clip_id);
    check(restored != nullptr, "clip");
    check(restored->media_id == media_id, "undo puts the clip back on the original");
}

// --- freeze frame ------------------------------------------------------------

NewMedia still_media()
{
    NewMedia still;
    still.path = "/freeze.jpg";
    still.name = "freeze.jpg";
    still.kind = MediaKind::Image;
    still.width = 1920;
    still.height = 1080;
    still.has_audio = false;
    return still;
}

void test_freeze_frame_holds_a_second_and_ripples_the_tail()
{
    auto [editor, media_id, clip_id] = fixture();

    const auto freeze =
            editor.apply(FreezeFrame{ clip_id, Rational{ 4, 1 }, Rational{ 1, 1 }, still_media() });
    check(freeze.has_value(), "freezes");
    check(freeze->created_id.has_value(), "freeze id");
    const std::string freeze_id = freeze->created_id.value_or("");

    const auto &clips = editor.project().active().clips;
    check(clips.size() == 3, "head + freeze + tail");
    const Clip *frozen = nullptr;
    const Clip *tail = nullptr;
    for (const Clip &clip : clips) {
        if (clip.id == freeze_id) {
            frozen = &clip;
        } else if (clip.id != clip_id) {
            tail = &clip;
        }
    }
    check(frozen != nullptr, "freeze");
    check(frozen->kind == ClipKind::Image, "the freeze is a still");
    check(frozen->start == Rational{ 4, 1 }, "the freeze sits at the playhead");
    check(frozen->duration == Rational{ 1, 1 }, "the freeze holds a second");
    check(tail != nullptr, "tail");
    check(tail->start == Rational{ 5, 1 }, "tail ripples by the hold");
    check(tail->source_start == Rational{ 4, 1 }, "the tail resumes at the cut");

    bool imported = false;
    for (const MediaItem &item : editor.project().media) {
        if (item.id != media_id && item.path == "/freeze.jpg") {
            imported = true;
        }
    }
    check(imported, "still is imported");
}

// A freeze cuts the clip the way a split does, so a curved clip has to
// come out of it the way a split leaves one: both pieces at the curve's
// constant mean, meeting at the frozen source time.
void test_freeze_frame_on_a_curved_clip_keeps_the_pieces_continuous()
{
    auto [editor, media_id, clip_id] = fixture();
    (void)media_id;

    std::vector<SpeedPoint> curve;
    curve.push_back(SpeedPoint{ 0.0, 0.5 });
    curve.push_back(SpeedPoint{ 1.0, 2.0 });
    const auto curved = editor.apply(SetClipSpeedCurve{ clip_id, std::move(curve) });
    check(curved.has_value(), "curves");

    const auto freeze =
            editor.apply(FreezeFrame{ clip_id, Rational{ 4, 1 }, Rational{ 1, 1 }, still_media() });
    check(freeze.has_value(), "freezes");
    check(freeze->created_id.has_value(), "freeze id");
    const std::string freeze_id = freeze->created_id.value_or("");

    const Timeline &timeline = editor.project().active();
    const Clip *head = timeline.clip(clip_id);
    check(head != nullptr, "head");
    const Clip *tail = nullptr;
    for (const Clip &clip : timeline.clips) {
        if (clip.id != clip_id && clip.id != freeze_id) {
            tail = &clip;
        }
    }
    check(tail != nullptr, "tail");
    check(!head->speed_curve.has_value(), "a piece kept a curve its in-point was not computed for");
    check(!tail->speed_curve.has_value(), "a piece kept a curve its in-point was not computed for");
    check_near(head->source_start.as_double() + head->duration.as_double() * head->speed,
               tail->source_start.as_double(), 1e-9, "the tail picks up where the head ends");
}

// --- split and merge ----------------------------------------------------------

void test_split_produces_source_continuous_halves_and_merge_rejoins_them()
{
    auto [editor, media_id, clip_id] = fixture();
    (void)media_id;
    const auto split =
            editor.apply(SplitClips{ std::vector<std::string>{ clip_id }, Rational{ 4, 1 } });
    check(split.has_value(), "splits");

    const auto &clips = editor.project().active().clips;
    check(clips.size() == 2, "two halves");
    check(clips[0].duration == Rational{ 4, 1 }, "the head is four seconds");
    check(clips[1].start == Rational{ 4, 1 }, "the tail starts where the head ends");
    check(clips[1].source_start == Rational{ 4, 1 }, "the tail resumes at the cut");

    std::vector<std::string> ids;
    for (const Clip &clip : clips) {
        ids.push_back(clip.id);
    }
    const auto merge = editor.apply(MergeClips{ std::move(ids) });
    check(merge.has_value(), "merges");
    const auto &merged = editor.project().active().clips;
    check(merged.size() == 1, "one clip again");
    check(merged[0].duration == Rational{ 10, 1 }, "the full length is back");
    check(merged[0].id == clip_id, "the first piece keeps its identity");
}

void test_a_head_trim_cannot_reach_before_the_source_begins()
{
    auto [editor, media_id, clip_id] = fixture();
    (void)media_id;
    const std::string track_id = editor.project().active().clips[0].track_id;
    const auto moved = editor.apply(
            MoveClips{ std::vector<ClipMove>{ ClipMove{ clip_id, Rational{ 5, 1 }, track_id } } });
    check(moved.has_value(), "moves");
    // Pulling the head four seconds left would want source -4: the trim
    // stops at the source's start, and the pixels do not slide.
    const auto trimmed = editor.apply(trim(clip_id, TrimEdge::Start, Rational{ -4, 1 }, false));
    check(trimmed.has_value(), "trims");
    const Clip *clip = editor.project().active().clip(clip_id);
    check(clip != nullptr, "clip");
    check(clip->start == Rational{ 5, 1 } && clip->duration == Rational{ 10, 1 }
                  && clip->source_start == Rational::ZERO,
          "the trim stops at the source's start");
}

// --- keys and effects ---------------------------------------------------------

void test_keys_stay_on_their_instant_through_split_trim_and_merge()
{
    auto [editor, media_id, clip_id] = fixture();
    (void)media_id;
    // Opacity 0 at the head, 1 at the middle (five seconds of a ten-second
    // clip), 0.5 at the end.
    const double ats[3] = { 0.0, 0.5, 1.0 };
    const double values[3] = { 0.0, 1.0, 0.5 };
    for (std::size_t i = 0; i < 3; ++i) {
        const auto keyed = editor.apply(
                SetClipKey{ clip_id, KeyProperty::Opacity, ats[i], values[i], KeyEase::linear() });
        check(keyed.has_value(), "keys");
    }
    const Clip *whole_ptr = editor.project().active().clip(clip_id);
    check(whole_ptr != nullptr, "clip");
    const Clip whole = *whole_ptr;
    const auto ride_at = [&whole](double seconds) {
        return whole.value_at(KeyProperty::Opacity, seconds / whole.duration.as_double());
    };

    const auto split =
            editor.apply(SplitClips{ std::vector<std::string>{ clip_id }, Rational{ 4, 1 } });
    check(split.has_value(), "splits");
    const Timeline &timeline = editor.project().active();
    const Clip head = timeline.clips[0];
    const Clip tail = timeline.clips[1];
    // The head's ride over its four seconds is the whole's over 0-4 s;
    // the tail's over its six is the whole's over 4-10 s.
    for (double seconds : { 0.0, 1.0, 2.5, 4.0 }) {
        const double got = head.value_at(KeyProperty::Opacity, seconds / head.duration.as_double());
        check_near(got, ride_at(seconds), 1e-9,
                   "head at " + std::to_string(seconds) + " rides like the whole");
    }
    for (double seconds : { 4.0, 5.0, 7.0, 10.0 }) {
        const double got =
                tail.value_at(KeyProperty::Opacity, (seconds - 4.0) / tail.duration.as_double());
        check_near(got, ride_at(seconds), 1e-9,
                   "tail at " + std::to_string(seconds) + " rides like the whole");
    }
    check(head.keys.size() == 2, "the head: its own key and one on the cut");
    check(tail.keys.size() == 3, "the tail: one on the cut and its own two");

    // Trimming a second off the tail's head keeps the middle key on the
    // picture's five-second mark: now at the tail's start.
    const auto trimmed_away = editor.apply(trim(tail.id, TrimEdge::Start, Rational{ 1, 1 }, false));
    check(trimmed_away.has_value(), "trims");
    const Clip *trimmed_ptr = editor.project().active().clip(tail.id);
    check(trimmed_ptr != nullptr, "tail");
    const Clip trimmed = *trimmed_ptr;
    const ClipKey *middle = nullptr;
    for (const ClipKey &key : trimmed.keys_on(KeyProperty::Opacity)) {
        if (std::abs(key.value - 1.0) < 1e-9) {
            middle = &key;
        }
    }
    check(middle != nullptr, "the middle key");
    check(middle != nullptr && std::abs(middle->at * trimmed.duration.as_double() - 0.0) < 1e-9,
          "the middle key lands on the picture's five-second mark");
    // And back, so the pieces join again.
    const auto trimmed_back =
            editor.apply(trim(tail.id, TrimEdge::Start, Rational{ -1, 1 }, false));
    check(trimmed_back.has_value(), "trims back");

    const auto merged = editor.apply(MergeClips{ std::vector<std::string>{ clip_id, tail.id } });
    check(merged.has_value(), "merges");
    const Clip *merged_ptr = editor.project().active().clip(clip_id);
    check(merged_ptr != nullptr, "merged");
    const Clip merged_clip = *merged_ptr;
    check(merged_clip.duration == Rational{ 10, 1 }, "the full length is back");
    for (double seconds : { 0.0, 2.0, 4.0, 5.0, 8.0, 10.0 }) {
        const double got = merged_clip.value_at(KeyProperty::Opacity,
                                                seconds / merged_clip.duration.as_double());
        check_near(got, ride_at(seconds), 1e-9,
                   "merged at " + std::to_string(seconds) + " rides like the whole");
    }

    // Lengthening the end spreads nothing: the keys keep their seconds.
    const auto extended = editor.apply(trim(clip_id, TrimEdge::End, Rational{ 10, 1 }, false));
    check(extended.has_value(), "extends");
    const Clip *longer_ptr = editor.project().active().clip(clip_id);
    check(longer_ptr != nullptr, "clip");
    const Clip longer = *longer_ptr;
    const ClipKey *middle_again = nullptr;
    for (const ClipKey &key : longer.keys_on(KeyProperty::Opacity)) {
        if (std::abs(key.value - 1.0) < 1e-9) {
            middle_again = &key;
        }
    }
    check(middle_again != nullptr, "the middle key");
    check(middle_again != nullptr
                  && std::abs(middle_again->at * longer.duration.as_double() - 5.0) < 1e-9,
          "the middle key stays on the picture's five-second mark");
}

void test_a_split_leaves_the_fade_in_with_the_head_and_the_fade_out_with_the_tail()
{
    auto [editor, media_id, clip_id] = fixture();
    (void)media_id;
    ClipPatch patch;
    patch.fade_in = Rational{ 1, 2 };
    patch.fade_out = Rational{ 1, 2 };
    const auto fades = editor.apply(UpdateClip{ clip_id, std::move(patch) });
    check(fades.has_value(), "fades");
    const auto split =
            editor.apply(SplitClips{ std::vector<std::string>{ clip_id }, Rational{ 4, 1 } });
    check(split.has_value(), "splits");
    const Timeline &timeline = editor.project().active();
    const Clip &head = timeline.clips[0];
    const Clip &tail = timeline.clips[1];
    check(head.fade_in == Rational{ 1, 2 } && head.fade_out == Rational::ZERO,
          "the fade-in stays with the head");
    check(tail.fade_in == Rational::ZERO && tail.fade_out == Rational{ 1, 2 },
          "the fade-out stays with the tail");

    std::vector<std::string> ids;
    for (const Clip &clip : timeline.clips) {
        ids.push_back(clip.id);
    }
    const auto merge = editor.apply(MergeClips{ std::move(ids) });
    check(merge.has_value(), "merges");
    const Clip &merged = editor.project().active().clips[0];
    check(merged.fade_in == Rational{ 1, 2 } && merged.fade_out == Rational{ 1, 2 },
          "both fades come back");
}

void test_effect_keys_stay_on_their_instant_through_a_split()
{
    auto [editor, media_id, clip_id] = fixture();
    (void)media_id;
    ClipPatch patch;
    patch.video_effects = std::vector<AppliedFilter>{ AppliedFilter::create("concat.vignette") };
    const auto applies = editor.apply(UpdateClip{ clip_id, std::move(patch) });
    check(applies.has_value(), "applies");
    const double ats[2] = { 0.0, 0.8 };
    const double values[2] = { 10.0, 90.0 };
    for (std::size_t i = 0; i < 2; ++i) {
        const auto keyed = editor.apply(
                SetEffectKey{ clip_id, 0, "strength", ats[i], values[i], KeyEase::linear() });
        check(keyed.has_value(), "keys");
    }
    const auto split =
            editor.apply(SplitClips{ std::vector<std::string>{ clip_id }, Rational{ 4, 1 } });
    check(split.has_value(), "splits");
    const Timeline &timeline = editor.project().active();
    const Clip &head = timeline.clips[0];
    const Clip &tail = timeline.clips[1];
    // The ride is 10 -> 90 over 0-8 s: 50 at the cut.
    const double at_cut_head = head.video_effects[0].value_at("strength", 1.0, 0.0);
    const double at_cut_tail = tail.video_effects[0].value_at("strength", 0.0, 0.0);
    check_near(at_cut_head, 50.0, 1e-9, "the ride is 50 at the cut's head end");
    check_near(at_cut_tail, 50.0, 1e-9, "the ride is 50 at the cut's tail start");
    // The tail's second key is still on the picture's 8 s: four seconds in.
    const ParamKey *ninety = nullptr;
    for (const ParamKey &key : tail.video_effects[0].keys_on("strength")) {
        if (std::abs(key.value - 90.0) < 1e-9) {
            ninety = &key;
        }
    }
    check(ninety != nullptr, "the 90 key");
    check(ninety != nullptr && std::abs(ninety->at * tail.duration.as_double() - 4.0) < 1e-9,
          "the 90 key stays on the picture's eight-second mark");
}

void test_rearranged_pieces_refuse_to_merge()
{
    auto [editor, media_id, clip_id] = fixture();
    (void)media_id;
    const auto split =
            editor.apply(SplitClips{ std::vector<std::string>{ clip_id }, Rational{ 4, 1 } });
    check(split.has_value(), "splits");
    const std::string tail_id = editor.project().active().clips[1].id;
    const std::string track_id = editor.project().active().tracks[0].id;
    // Swap the two pieces on the timeline: adjacent, but out of order.
    const auto moved = editor.apply(MoveClips{ std::vector<ClipMove>{
            ClipMove{ clip_id, Rational{ 6, 1 }, track_id },
            ClipMove{ tail_id, Rational::ZERO, track_id },
    } });
    check(moved.has_value(), "moves");
    const auto refused = editor.apply(MergeClips{ std::vector<std::string>{ tail_id, clip_id } });
    check(!refused.has_value(), "refused");
    check(refused.error().kind == CommandErrorKind::CannotMerge, "refused as not mergeable");
    check(refused.error().reason.find("no longer in their original order") != std::string::npos,
          "the pieces are out of order");
}

void test_a_head_trim_moves_the_in_point_with_speed()
{
    auto [editor, media_id, clip_id] = fixture();
    (void)media_id;
    const auto sped = editor.apply(SetClipSpeed{ clip_id, 2.0 });
    check(sped.has_value(), "sets the speed");
    check(editor.project().active().clips[0].duration == Rational{ 5, 1 },
          "ten seconds of source at twice the speed is five on the timeline");
    const auto trimmed = editor.apply(trim(clip_id, TrimEdge::Start, Rational{ 1, 1 }, false));
    check(trimmed.has_value(), "trims");
    const Clip &clip = editor.project().active().clips[0];
    check(clip.start == Rational{ 1, 1 }, "the head moves");
    check(clip.duration == Rational{ 4, 1 }, "the duration shortens");
    check(clip.source_start == Rational{ 2, 1 }, "a timeline second covers two source seconds");
}

void test_speed_clamps_to_the_engine_range()
{
    auto [editor, media_id, clip_id] = fixture();
    (void)media_id;
    const auto fast = editor.apply(SetClipSpeed{ clip_id, 100.0 });
    check(fast.has_value(), "sets");
    check_near(editor.project().active().clips[0].speed, genesis::core::SpeedCurve::MAX_SPEED,
               1e-12, "a hundred clamps to the engine's ceiling");
    const auto slow = editor.apply(SetClipSpeed{ clip_id, 0.0 });
    check(slow.has_value(), "sets");
    check_near(editor.project().active().clips[0].speed, genesis::core::SpeedCurve::MIN_SPEED,
               1e-12, "zero clamps to the engine's floor");
}

void test_set_clip_reverse_flips_playback_direction_and_undoes()
{
    auto [editor, media_id, clip_id] = fixture();
    (void)media_id;
    const Clip *before = editor.project().active().clip(clip_id);
    check(before != nullptr && !before->reverse, "a clip starts forwards");

    const auto reversed = editor.apply(SetClipReverse{ clip_id, true });
    check(reversed.has_value(), "reverses");
    check(reversed->applied, "applied");
    check(editor.project().active().clip(clip_id)->reverse, "the clip plays backwards");

    // Setting the same value again changes nothing, so it is not an undo
    // step.
    const bool undoable = editor.can_undo();
    const auto again = editor.apply(SetClipReverse{ clip_id, true });
    check(again.has_value() && !again->applied, "same value is a no-op");
    check(editor.can_undo() == undoable, "a no-op is not a step");

    // An unknown clip is a tolerated no-op.
    const auto nobody = editor.apply(SetClipReverse{ "c999", true });
    check(nobody.has_value() && !nobody->applied, "a missing clip is a no-op");

    editor.undo();
    check(!editor.project().active().clip(clip_id)->reverse, "undo plays forwards again");
}

void test_set_clip_pan_clamps_and_undoes()
{
    auto [editor, media_id, clip_id] = fixture();
    (void)media_id;
    const Clip *before = editor.project().active().clip(clip_id);
    check(before != nullptr && before->pan == 0.0, "a clip starts centred");

    const auto panned = editor.apply(SetClipPan{ clip_id, 0.75 });
    check(panned.has_value(), "pans");
    check(panned->applied, "applied");
    check_near(editor.project().active().clip(clip_id)->pan, 0.75, 1e-12, "the pan lands");

    // Out of range clamps rather than refuses.
    const auto clamped = editor.apply(SetClipPan{ clip_id, 2.0 });
    check(clamped.has_value() && clamped->applied, "clamps");
    check_near(editor.project().active().clip(clip_id)->pan, 1.0, 1e-12,
               "a pan past one clamps to it");

    // Setting the same value again changes nothing, so it is not an undo
    // step.
    const bool undoable = editor.can_undo();
    const auto again = editor.apply(SetClipPan{ clip_id, 1.0 });
    check(again.has_value() && !again->applied, "same value is a no-op");
    check(editor.can_undo() == undoable, "a no-op is not a step");

    // An unknown clip is a tolerated no-op.
    const auto nobody = editor.apply(SetClipPan{ "c999", 0.5 });
    check(nobody.has_value() && !nobody->applied, "a missing clip is a no-op");

    // Undo walks both the clamp and the pan back to centred.
    editor.undo();
    editor.undo();
    check_near(editor.project().active().clip(clip_id)->pan, 0.0, 1e-12,
               "two undos return to centred");
}

// --- detach and reattach audio ----------------------------------------------

void test_a_video_whose_detached_sound_was_deleted_can_be_unmuted()
{
    auto [editor, media_id, clip_id] = fixture();
    (void)media_id;
    check(editor.apply(DetachAudio{ clip_id }).has_value(), "detaches");
    std::vector<std::string> sound;
    for (const Clip &clip : editor.project().active().clips) {
        if (clip.id != clip_id) {
            sound.push_back(clip.id);
        }
    }
    check(sound.size() == 1, "one sound clip");
    check(editor.apply(RemoveClips{ std::move(sound), false }).has_value(), "removes");
    const Clip *video = editor.project().active().clip(clip_id);
    check(video != nullptr && video->muted.has_value() && *video->muted == true,
          "the video stays muted");

    ClipPatch patch;
    patch.muted = false;
    check(editor.apply(UpdateClip{ clip_id, std::move(patch) }).has_value(), "unmutes");
    const Clip *unmuted = editor.project().active().clip(clip_id);
    check(unmuted != nullptr && !unmuted->muted.has_value(), "unmuted is the absent value");
}

void test_detach_and_reattach_round_trip_the_sound()
{
    auto [editor, media_id, clip_id] = fixture();
    (void)media_id;
    const auto detached = editor.apply(DetachAudio{ clip_id });
    check(detached.has_value(), "detaches");
    check(detached->created_id.has_value(), "sound clip");
    const std::string sound_id = detached->created_id.value_or("");

    const Timeline &timeline = editor.project().active();
    check(timeline.clips.size() == 2, "two clips");
    const Clip *sound = timeline.clip(sound_id);
    check(sound != nullptr, "the sound exists");
    check(sound != nullptr && sound->kind == ClipKind::Audio, "the sound is audio");
    check(sound != nullptr && sound->detached_from.has_value() && *sound->detached_from == clip_id,
          "the sound remembers its video");
    const Clip *video = timeline.clip(clip_id);
    check(video != nullptr && video->muted.has_value() && *video->muted == true,
          "the video is muted");

    check(editor.apply(ReattachAudio{ sound_id }).has_value(), "reattaches");
    const Timeline &after = editor.project().active();
    check(after.clips.size() == 1, "one clip again");
    const Clip *restored = after.clip(clip_id);
    check(restored != nullptr && !restored->muted.has_value(), "the video is unmuted");
}

void test_detaching_a_two_track_recording_gives_one_sound_clip_per_track()
{
    Editor editor;
    const auto added = editor.apply(two_track_media("/rec.mkv"));
    check(added.has_value(), "adds");
    check(added->created_id.has_value(), "id");
    const std::string media_id = added->created_id.value_or("");
    check(editor.project().media[0].audio_tracks.size() == 2, "two streams");
    const std::string track_id = editor.project().active().tracks[0].id;
    const auto placed = editor.apply(AddClip{ media_id, track_id, Rational::ZERO, false });
    check(placed.has_value(), "adds");
    check(placed->created_id.has_value(), "id");
    const std::string clip_id = placed->created_id.value_or("");

    check(editor.apply(DetachAudio{ clip_id }).has_value(), "detaches");
    const Timeline &timeline = editor.project().active();
    std::vector<const Clip *> sounds;
    for (const Clip &clip : timeline.clips) {
        if (clip.kind == ClipKind::Audio) {
            sounds.push_back(&clip);
        }
    }
    check(sounds.size() == 2, "one sound clip per track");
    check(sounds.size() == 2 && sounds[0]->audio_stream.has_value()
                  && *sounds[0]->audio_stream == 1,
          "the first plays stream one");
    check(sounds.size() == 2 && sounds[1]->audio_stream.has_value()
                  && *sounds[1]->audio_stream == 2,
          "the second plays stream two");
    check(sounds.size() == 2 && sounds[0]->name.ends_with("Track 1"),
          "the first is named for its track");
    check(sounds.size() == 2 && sounds[1]->name.ends_with("Mic/Aux"),
          "the second is named for its title");
    check(sounds.size() == 2 && sounds[0]->track_id != sounds[1]->track_id, "each on its own lane");
    const bool all_detached =
            std::all_of(sounds.begin(), sounds.end(), [&clip_id](const Clip *sound) {
                return sound->detached_from.has_value() && *sound->detached_from == clip_id;
            });
    check(all_detached, "each remembers its video");

    check(editor.apply(ReattachAudio{ sounds[1]->id }).has_value(), "reattaches");
    check(editor.project().active().clips.size() == 1, "one clip again");
    const Clip *video = editor.project().active().clip(clip_id);
    check(video != nullptr && !video->muted.has_value(), "the video is unmuted");

    ClipPatch picks;
    picks.audio_stream = std::optional<std::uint32_t>{ 2 };
    check(editor.apply(UpdateClip{ clip_id, std::move(picks) }).has_value(), "picks");
    const Clip *picked = editor.project().active().clip(clip_id);
    check(picked != nullptr && picked->audio_stream.has_value() && *picked->audio_stream == 2,
          "the video plays stream two");

    ClipPatch forgets;
    forgets.audio_stream = std::optional<std::uint32_t>{ };
    check(editor.apply(UpdateClip{ clip_id, std::move(forgets) }).has_value(), "forgets");
    const Clip *forgot = editor.project().active().clip(clip_id);
    check(forgot != nullptr && !forgot->audio_stream.has_value(), "back to the file's first");
}

// --- non-finite numbers ------------------------------------------------------

void test_a_command_carrying_nan_is_refused_whole()
{
    auto [editor, media_id, clip_id] = fixture();
    (void)media_id;
    Project before = editor.project();
    const double nan = std::numeric_limits<double>::quiet_NaN();
    const double inf = std::numeric_limits<double>::infinity();
    // `time` and `delta` are Rationals in C++ (no NaN), so the refused
    // numbers ride doubles: a key's `at`, and a speed inside a Batch.
    const std::vector<Command> commands = {
        Command{ SetClipSpeed{ clip_id, nan } },
        Command{ SetClipPan{ clip_id, nan } },
        Command{ SetClipKey{ clip_id, KeyProperty::Opacity, nan, 1.0, KeyEase::linear() } },
        Command{ SetClipTransform{ clip_id, std::nullopt, inf, std::nullopt, std::nullopt,
                                   std::nullopt, std::nullopt } },
        Command{ Batch{ std::vector<Command>{ Command{ SetClipSpeed{ clip_id, nan } } } } },
    };
    for (const Command &command : commands) {
        const auto result = editor.apply(command);
        check(!result.has_value(), "refused");
        if (!result.has_value()) {
            check(result.error().kind == CommandErrorKind::NotANumber, "refused as not a number");
        }
    }
    check(editor.project() == before, "nothing changed");
}

void test_nan_in_transition_or_text_patch_is_refused()
{
    auto [editor, media_id, clip_id] = fixture();
    (void)media_id;
    Project before = editor.project();
    const double nan = std::numeric_limits<double>::quiet_NaN();
    const double inf = std::numeric_limits<double>::infinity();

    // A transition's duration is a Rational in C++ (no NaN), so the refused
    // number rides the patch's volume, a double.
    ClipPatch bad_volume;
    bad_volume.volume = nan;
    const auto refused_volume = editor.apply(UpdateClip{ clip_id, std::move(bad_volume) });
    check(!refused_volume.has_value(), "refused");
    if (!refused_volume.has_value()) {
        check(refused_volume.error().kind == CommandErrorKind::NotANumber,
              "refused as not a number");
    }

    TextStyle style;
    style.font_size = inf;
    ClipPatch bad_text;
    bad_text.text = style;
    const auto refused_text = editor.apply(UpdateClip{ clip_id, std::move(bad_text) });
    check(!refused_text.has_value(), "refused");
    if (!refused_text.has_value()) {
        check(refused_text.error().kind == CommandErrorKind::NotANumber, "refused as not a number");
    }
    check(editor.project() == before, "nothing changed");
}

// --- timelines ----------------------------------------------------------------

void test_timelines_add_switch_and_delete_with_a_floor_of_one()
{
    Editor editor;
    const auto added = editor.apply(AddTimeline{ });
    check(added.has_value(), "adds");
    check(added->created_id.has_value(), "id");
    const std::string second = added->created_id.value_or("");
    check(editor.project().active_timeline_id == second, "the new timeline is active");
    check(editor.project().timelines[1].name == "Timeline 2", "named after the count");
    const bool fresh_lanes = std::all_of(editor.project().timelines[1].tracks.begin(),
                                         editor.project().timelines[1].tracks.end(),
                                         [](const Track &track) { return track.id != "T1"; });
    check(fresh_lanes, "fresh lanes must not reuse the first timeline's ids");

    check(editor.apply(SelectTimeline{ "TL1" }).has_value(), "selects");
    check(editor.project().active_timeline_id == "TL1", "back on the first");

    check(editor.apply(RemoveTimeline{ "TL1" }).has_value(), "removes");
    check(editor.project().active_timeline_id == second, "the active tab moves to its neighbour");

    const std::string last = editor.project().timelines[0].id;
    const auto refused = editor.apply(RemoveTimeline{ last });
    check(!refused.has_value(), "the last timeline cannot be deleted");
    if (!refused.has_value()) {
        check(refused.error().kind == CommandErrorKind::LastTimeline,
              "refused at the floor of one");
    }
}

void test_a_timeline_moves_to_a_new_slot_without_stealing_the_selection()
{
    Editor editor;
    const auto add_second = editor.apply(AddTimeline{ });
    check(add_second.has_value() && add_second->created_id.has_value(), "id");
    const std::string second = add_second->created_id.value_or("");
    const auto add_third = editor.apply(AddTimeline{ });
    check(add_third.has_value() && add_third->created_id.has_value(), "id");
    const std::string third = add_third->created_id.value_or("");
    check(editor.apply(SelectTimeline{ "TL1" }).has_value(), "selects");

    const auto order = [&editor]() {
        std::vector<std::string> ids;
        for (const Timeline &timeline : editor.project().timelines) {
            ids.push_back(timeline.id);
        }
        return ids;
    };

    check(editor.apply(MoveTimeline{ third, 0 }).has_value(), "moves");
    check(order() == std::vector<std::string>{ third, "TL1", second }, "third moved to the front");
    check(editor.project().active_timeline_id == "TL1", "moving must not select");

    check(editor.apply(MoveTimeline{ third, 99 }).has_value(), "clamps");
    check(order() == std::vector<std::string>{ "TL1", second, third },
          "an index past the end lands last");

    const bool before_can_undo = editor.can_undo();
    const auto noop = editor.apply(MoveTimeline{ "nope", 0 });
    check(noop.has_value(), "tolerated");
    check(noop.has_value() && !noop->applied, "a no-op changes nothing");
    check(editor.can_undo() == before_can_undo, "a no-op is not a step");
}

// --- undo and redo -----------------------------------------------------------

void test_undo_and_redo_walk_the_history()
{
    auto [editor, media_id, clip_id] = fixture();
    (void)media_id;
    const auto split =
            editor.apply(SplitClips{ std::vector<std::string>{ clip_id }, Rational{ 5, 1 } });
    check(split.has_value(), "splits");
    check(editor.project().active().clips.size() == 2, "two halves");

    editor.undo();
    check(editor.project().active().clips.size() == 1, "undo rejoins");
    editor.redo();
    check(editor.project().active().clips.size() == 2, "redo splits again");
    editor.undo();
    editor.undo();
    editor.undo();
    check(editor.project().media.empty(), "all the way back to empty");
}

void test_a_failed_command_records_no_history()
{
    auto [editor, media_id, clip_id] = fixture();
    (void)media_id;
    (void)clip_id;
    const bool before_can_undo = editor.can_undo();
    (void)editor.apply(MergeClips{ std::vector<std::string>{ "nope" } });
    check(editor.can_undo() == before_can_undo, "a failure is not a step");
}

// --- project shape -----------------------------------------------------------

void test_a_new_project_has_one_timeline_and_four_lanes()
{
    Editor editor;
    check(editor.project().timelines.size() == 1, "one timeline");
    check(editor.project().active().tracks.size() == 4, "four lanes");
}

void test_duplicate_media_paths_are_ignored()
{
    Editor editor;
    check(editor.apply(media("/a.mp4", Rational{ 10, 1 }, true)).has_value(), "adds");
    check(editor.apply(media("/a.mp4", Rational{ 10, 1 }, true)).has_value(), "no-op");
    check(editor.project().media.size() == 1, "one bin item");
}

// --- gestures and batch history ----------------------------------------------

void test_a_gesture_is_one_undo_step()
{
    auto [editor, media_id, clip_id] = fixture();
    (void)media_id;
    const double start = editor.project().active().clips[0].scale;
    for (double scale : { 1.1, 1.2, 1.3 }) {
        const auto scaled = editor.apply_within(
                std::optional<std::string>{ "scale" },
                Command{ SetClipTransform{ clip_id, scale, std::nullopt, std::nullopt, std::nullopt,
                                           std::nullopt, std::nullopt } });
        check(scaled.has_value(), "scales");
    }
    editor.end_gesture();
    const auto again = editor.apply_within(
            std::optional<std::string>{ "scale" },
            Command{ SetClipTransform{ clip_id, 2.0, std::nullopt, std::nullopt, std::nullopt,
                                       std::nullopt, std::nullopt } });
    check(again.has_value(), "scales again");
    check(editor.project().active().clips[0].scale == 2.0, "the second drag landed");
    editor.undo();
    check(editor.project().active().clips[0].scale == 1.3, "undo lands at the first drag's end");
    editor.undo();
    check(editor.project().active().clips[0].scale == start, "undo lands where the drag began");
    editor.redo();
    check(editor.project().active().clips[0].scale == 1.3, "redo lands at the first drag's end");
}

void test_switching_and_reordering_tabs_is_not_an_edit()
{
    auto [editor, media_id, clip_id] = fixture();
    (void)media_id;
    (void)clip_id;
    check(editor.apply(AddTimeline{ }).has_value(), "adds");
    const std::string first = editor.project().timelines[0].id;
    const std::string second = editor.project().timelines[1].id;
    const auto count_steps = [&editor]() {
        int count = 0;
        while (editor.can_undo()) {
            editor.undo();
            ++count;
        }
        while (editor.can_redo()) {
            editor.redo();
        }
        return count;
    };
    const int steps_before = count_steps();
    check(editor.apply(SelectTimeline{ first }).has_value(), "selects");
    check(editor.apply(MoveTimeline{ second, 0 }).has_value(), "moves");
    check(editor.project().active_timeline_id == first, "the selection stays");
    check(editor.project().timelines[0].id == second, "the tab order moved");
    check(count_steps() == steps_before, "neither the switch nor the move was a step");
}

// A ripple delete moves the clips behind the gap alone: the clip in front
// keeps its identity, and only the moved clip changes position.
void test_a_ripple_copies_only_the_clips_it_moves()
{
    auto [editor, media_id, clip_id] = fixture();
    (void)media_id;
    check(editor.apply(SplitClips{ std::vector<std::string>{ clip_id }, Rational{ 4, 1 } })
                  .has_value(),
          "splits");
    check(editor.apply(SplitClips{
                               std::vector<std::string>{ editor.project().active().clips[1].id },
                               Rational{ 7, 1 } })
                  .has_value(),
          "splits again");
    const std::string head = editor.project().active().clips[0].id;
    const std::string middle = editor.project().active().clips[1].id;
    const std::string last = editor.project().active().clips[2].id;
    check(editor.apply(RemoveClips{ std::vector<std::string>{ middle }, true }).has_value(),
          "ripple deletes");
    const auto &clips = editor.project().active().clips;
    check(clips.size() == 2, "two clips remain");
    const Clip *front = editor.project().active().clip(head);
    check(front != nullptr && front->start == Rational::ZERO && front->duration == Rational{ 4, 1 },
          "the clip in front of the gap is untouched");
    const Clip *moved = editor.project().active().clip(last);
    check(moved != nullptr && moved->start == Rational{ 4, 1 }, "the moved clip closes the gap");
}

void test_a_command_leaves_a_tidy_clip_behind()
{
    auto [editor, media_id, clip_id] = fixture();
    (void)media_id;
    const auto keyed = editor.apply(
            Command{ SetClipKey{ clip_id, KeyProperty::OffsetX, 0.5, 99.0, KeyEase::linear() } });
    check(keyed.has_value(), "sets a key");
    const Clip &clip = editor.project().active().clips[0];
    const auto keys = clip.keys_on(KeyProperty::OffsetX);
    check(!keys.empty(), "the key");
    check(!keys.empty() && keys[0].value == 3.0, "clamped like the field");
}

void test_a_command_copies_only_what_it_writes()
{
    auto [editor, media_id, clip_id] = fixture();
    (void)media_id;
    check(editor.apply(SplitClips{ std::vector<std::string>{ clip_id }, Rational{ 4, 1 } })
                  .has_value(),
          "splits");
    check(editor.apply(AddTimeline{ }).has_value(), "adds");
    check(editor.apply(SelectTimeline{ "TL1" }).has_value(), "back to the first");
    const Timeline other_before = editor.project().timelines[1];
    const Clip untouched_before = editor.project().active().clips[1];
    ClipPatch patch;
    patch.volume = 0.5;
    check(editor.apply(UpdateClip{ clip_id, std::move(patch) }).has_value(), "edits the head");
    check(editor.project().timelines[1] == other_before, "the other timeline is untouched");
    check(editor.project().active().clips[1] == untouched_before,
          "the clip the edit did not touch is untouched");
    check(editor.project().active().clips[0].volume == 0.5, "the head is edited");
    editor.undo();
    check(editor.project().active().clips[0].volume == 1.0, "undo restores the head");
}

void test_a_batch_is_one_undo_step()
{
    auto [editor, media_id, clip_id] = fixture();
    (void)media_id;
    const auto batched = editor.apply(Command{ Batch{ std::vector<Command>{
            Command{ SplitClips{ std::vector<std::string>{ clip_id }, Rational{ 4, 1 } } },
            Command{ SetClipSpeed{ clip_id, 2.0 } },
    } } });
    check(batched.has_value(), "applies");
    check(editor.project().active().clips.size() == 2, "two clips");
    check(editor.project().active().clips[0].speed == 2.0, "the speed lands");
    editor.undo();
    const auto &clips = editor.project().active().clips;
    check(clips.size() == 1, "one undo reverses the whole batch");
    check(clips[0].speed == 1.0, "the speed is back");
}

void test_a_failed_batch_changes_nothing()
{
    auto [editor, media_id, clip_id] = fixture();
    (void)media_id;
    const Project before = editor.project();
    const auto refused = editor.apply(Command{ Batch{ std::vector<Command>{
            Command{ SplitClips{ std::vector<std::string>{ clip_id }, Rational{ 4, 1 } } },
            Command{ MergeClips{ std::vector<std::string>{ "nope" } } },
    } } });
    check(!refused.has_value(), "refused");
    check(editor.project() == before, "the successful half was rolled back");
    editor.undo();
    check(editor.project().active().clips.empty(),
          "the first undo steps over the fixture's add-clip, so the batch "
          "recorded nothing");
}

void test_undo_depth_evicts_the_oldest_snapshot_first()
{
    auto [editor, media_id, clip_id] = fixture();
    (void)media_id;
    for (int step = 0; step < 205; ++step) {
        const double offset = static_cast<double>(step) / 1000.0 + 0.001;
        const auto applied = editor.apply(
                Command{ SetClipTransform{ clip_id, std::nullopt, offset, std::nullopt,
                                           std::nullopt, std::nullopt, std::nullopt } });
        check(applied.has_value(), "applies");
    }
    std::size_t undos = 0;
    while (editor.can_undo()) {
        editor.undo();
        ++undos;
    }
    check(undos == History::DEPTH, "the newest DEPTH steps stay undoable");
}

// --- tracks, media, timelines and transforms ---------------------------------

void test_setting_a_value_already_in_place_records_no_undo_entry()
{
    auto [editor, media_id, clip_id] = fixture();
    (void)media_id;
    // Volume is already 1.0; the old deep-compare saw no change here and
    // pushed nothing - the applied flag must agree exactly.
    ClipPatch patch;
    patch.volume = 1.0;
    const auto outcome = editor.apply(UpdateClip{ clip_id, std::move(patch) });
    check(outcome.has_value(), "tolerated");
    check(!outcome->applied, "already the value");
    editor.undo();
    check(editor.project().active().clips.empty(), "straight back past the add-clip");
}

void test_a_batch_reports_applied_only_when_a_member_changed_something()
{
    auto [editor, media_id, clip_id] = fixture();
    (void)media_id;
    const std::string track_id = editor.project().active().tracks[0].id;
    // Visible is already true; only the speed change is real.
    const auto outcome = editor.apply(Command{ Batch{ std::vector<Command>{
            Command{ SetTrackFlag{ track_id, TrackFlag::Visible, true } },
            Command{ SetClipSpeed{ clip_id, 2.0 } },
    } } });
    check(outcome.has_value(), "applies");
    check(outcome->applied, "a member changed something");
    editor.undo();
    check(editor.project().active().clips[0].speed == 1.0, "one undo undoes the batch");

    // A batch of nothing-but-no-ops is itself a no-op: no undo entry.
    const auto noop = editor.apply(Command{ Batch{ std::vector<Command>{
            Command{ SetTrackFlag{ track_id, TrackFlag::Visible, true } },
            Command{ TrimClip{ "nope", TrimEdge::End, Rational{ 1, 1 }, false } },
    } } });
    check(noop.has_value(), "tolerated");
    check(!noop->applied, "a batch of no-ops is a no-op");
    editor.undo();
    check(editor.project().active().clips.empty(),
          "the next undo steps over the add-clip, not a phantom batch");
}

void test_a_batch_imports_and_places_media_in_one_step()
{
    // A fresh editor, exactly as the demo builds one: the mint counter is not
    // adopted from the founding timeline's T1..T4 lanes, so a scan of the
    // project's numeric suffixes would read 4 and guess "m5". peek_id reads the
    // mint itself, so it says "m1" - the id AddMedia will actually mint.
    Editor editor;
    check(editor.peek_id("m") == "m1", "peek_id reads the mint, not the lanes");

    NewMedia item;
    item.path = "/speech/read.wav";
    item.name = "read.wav";
    item.duration = Rational{ 3, 1 };
    item.kind = MediaKind::Audio;
    item.has_audio = true;
    item.origin = MediaOrigin::Speech;

    // The id the next AddMedia will mint, read before the batch so the paired
    // AddClip can name it.
    const std::string media_id = editor.peek_id("m");
    const std::string track_id = editor.project().active().tracks[0].id;

    const auto outcome = editor.apply(Command{ Batch{ std::vector<Command>{
            Command{ AddMedia{ std::move(item) } },
            Command{ AddClip{ media_id, track_id, Rational::ZERO, false } },
    } } });
    check(outcome.has_value(), "the import batch applies");
    check(editor.project().media_by_id(media_id) != nullptr, "the media lands");
    check(editor.project().active().clips.size() == 1, "the clip lands");
    check(editor.project().active().clips[0].media_id == media_id,
          "the clip references the peeked media id");

    // One undo removes the clip and the media together.
    editor.undo();
    check(editor.project().active().clips.empty(), "undo removes the clip");
    check(editor.project().media_by_id(media_id) == nullptr, "undo removes the media too");

    // Redo restores both, not just the last member.
    editor.redo();
    check(editor.project().media_by_id(media_id) != nullptr, "redo restores the media");
    check(editor.project().active().clips.size() == 1, "redo restores the clip");
}

void test_moves_to_a_vanished_track_keep_the_time_change_and_drop_the_track_change()
{
    auto [editor, media_id, clip_id] = fixture();
    (void)media_id;
    const auto outcome = editor.apply(MoveClips{ std::vector<ClipMove>{
            ClipMove{ clip_id, Rational{ 3, 1 }, "gone" },
    } });
    check(outcome.has_value(), "tolerated");
    check(outcome->applied, "the start change is real");
    const Clip *clip = editor.project().active().clip(clip_id);
    check(clip != nullptr, "exists");
    check(clip != nullptr && clip->start == Rational{ 3, 1 }, "moved in time");
    check(clip != nullptr && clip->track_id == "T1", "the vanished destination is ignored");
}

void test_a_multi_move_skips_unknown_clips_and_floors_start_at_zero()
{
    auto [editor, media_id, clip_id] = fixture();
    (void)media_id;
    const std::string target = editor.project().active().tracks[1].id;
    // The unknown clip is skipped; the rest still lands.
    const auto outcome = editor.apply(MoveClips{ std::vector<ClipMove>{
            ClipMove{ "nope", Rational{ 9, 1 }, target },
            ClipMove{ clip_id, Rational{ -2, 1 }, target },
    } });
    check(outcome.has_value(), "tolerated");
    const Clip *clip = editor.project().active().clip(clip_id);
    check(clip != nullptr, "exists");
    check(clip != nullptr && clip->start == Rational::ZERO,
          "negative starts clamp to the timeline head");
    check(clip != nullptr && clip->track_id == target, "moved lanes");
}

void test_a_tail_trim_stretches_only_the_duration_and_stops_at_the_minimum()
{
    auto [editor, media_id, clip_id] = fixture();
    (void)media_id;
    check(editor.apply(trim(clip_id, TrimEdge::End, Rational{ -4, 1 }, false)).has_value(),
          "trims");
    const Clip *clip = editor.project().active().clip(clip_id);
    check(clip != nullptr, "exists");
    check(clip != nullptr && clip->duration == Rational{ 6, 1 }, "shortened to six");
    check(clip != nullptr && clip->start == Rational::ZERO, "the head does not move");
    check(clip != nullptr && clip->source_start == Rational::ZERO, "nor the in-point");

    // Dragging far past the head floors at a sixtieth of a second rather
    // than inverting the clip.
    check(editor.apply(trim(clip_id, TrimEdge::End, Rational{ -100, 1 }, false)).has_value(),
          "trims");
    const Clip *floored = editor.project().active().clip(clip_id);
    check(floored != nullptr, "exists");
    check(floored != nullptr && floored->duration == Rational{ 1, 60 }, "floored at a sixtieth");
}

void test_add_at_first_free_takes_the_lowest_empty_lane_or_the_bottom_one()
{
    auto [editor, media_id, clip_id] = fixture();
    (void)clip_id;
    // Track one is occupied for [0, 10), so the same span lands on two.
    const auto second = editor.apply(AddClipAtFirstFree{ media_id, Rational::ZERO });
    check(second.has_value(), "adds");
    check(second->created_id.has_value(), "id");
    const std::string second_id = second->created_id.value_or("");
    const Clip *second_clip = editor.project().active().clip(second_id);
    check(second_clip != nullptr, "exists");
    check(second_clip != nullptr && second_clip->track_id == editor.project().active().tracks[1].id,
          "track one occupied, so the span lands on two");

    // Fill the remaining lanes, then ask again: rather than refusing, the
    // clip overlaps on the first track.
    check(editor.apply(AddClipAtFirstFree{ media_id, Rational::ZERO }).has_value(), "adds");
    check(editor.apply(AddClipAtFirstFree{ media_id, Rational::ZERO }).has_value(), "adds");
    const auto overflow = editor.apply(AddClipAtFirstFree{ media_id, Rational::ZERO });
    check(overflow.has_value(), "adds");
    check(overflow->created_id.has_value(), "id");
    const std::string overflow_id = overflow->created_id.value_or("");
    const Clip *overflow_clip = editor.project().active().clip(overflow_id);
    check(overflow_clip != nullptr, "exists");
    check(overflow_clip != nullptr
                  && overflow_clip->track_id == editor.project().active().tracks[0].id,
          "a full timeline falls back to the first track, overlap and all");

    // A clear span later in time finds track one free again.
    const auto clear = editor.apply(AddClipAtFirstFree{ media_id, Rational{ 20, 1 } });
    check(clear.has_value(), "adds");
    check(clear->created_id.has_value(), "id");
    const std::string clear_id = clear->created_id.value_or("");
    const Clip *clear_clip = editor.project().active().clip(clear_id);
    check(clear_clip != nullptr, "exists");
    check(clear_clip != nullptr && clear_clip->track_id == editor.project().active().tracks[0].id,
          "a clear span later finds track one free again");
}

void test_removing_a_track_takes_its_clips_and_stops_at_the_floor_of_one()
{
    auto [editor, media_id, clip_id] = fixture();
    (void)media_id;
    (void)clip_id;
    const std::string track_id = editor.project().active().tracks[0].id;
    check(editor.apply(RemoveTrack{ track_id }).has_value(), "removes");
    const Timeline &timeline = editor.project().active();
    check(timeline.tracks.size() == 3, "three lanes left");
    check(timeline.clips.empty(), "the clip on the removed track went with it");

    // An unknown id is the tolerated no-op, not an error.
    const auto outcome = editor.apply(RemoveTrack{ track_id });
    check(outcome.has_value(), "tolerated");
    check(!outcome->applied, "an unknown id is a no-op");

    while (editor.project().active().tracks.size() > 1) {
        const std::string next = editor.project().active().tracks[0].id;
        check(editor.apply(RemoveTrack{ next }).has_value(), "removes");
    }
    const std::string last = editor.project().active().tracks[0].id;
    const auto refused = editor.apply(RemoveTrack{ last });
    check(!refused.has_value(), "refused");
    check(!refused.has_value() && refused.error().kind == CommandErrorKind::LastTrack,
          "refused at the floor of one");
}

void test_removing_media_sweeps_its_clips_from_every_timeline()
{
    auto [editor, media_id, clip_id] = fixture();
    (void)clip_id;
    check(editor.apply(AddTimeline{ }).has_value(), "adds");
    const std::string track_id = editor.project().active().tracks[0].id;
    check(editor.apply(AddClip{ media_id, track_id, Rational::ZERO, false }).has_value(), "adds");

    check(editor.apply(RemoveMedia{ media_id }).has_value(), "removes");
    check(editor.project().media.empty(), "the bin is empty");
    const bool all_swept =
            std::all_of(editor.project().timelines.begin(), editor.project().timelines.end(),
                        [](const Timeline &timeline) { return timeline.clips.empty(); });
    check(all_swept, "the inactive timeline's clip is swept too, not left dangling");

    // Removing what is already gone is a tolerated no-op.
    const auto outcome = editor.apply(RemoveMedia{ media_id });
    check(outcome.has_value(), "tolerated");
    check(!outcome->applied, "removing what is gone is a no-op");
}

void test_renames_trim_whitespace_and_refuse_to_blank_a_name()
{
    auto [editor, media_id, clip_id] = fixture();
    (void)media_id;
    (void)clip_id;
    check(editor.apply(RenameTimeline{ "TL1", "\tCut A\n" }).has_value(), "renames");
    check(editor.project().timelines[0].name == "Cut A", "trimmed");
    const auto outcome = editor.apply(RenameTimeline{ "TL1", "" });
    check(outcome.has_value(), "tolerated");
    check(!outcome->applied, "a blank name is a no-op");
    check(editor.project().timelines[0].name == "Cut A", "the name stays");
}

void test_track_flags_flip_independently_and_report_no_ops_honestly()
{
    auto [editor, media_id, clip_id] = fixture();
    (void)media_id;
    (void)clip_id;
    const std::string track_id = editor.project().active().tracks[0].id;
    check(editor.apply(SetTrackFlag{ track_id, TrackFlag::Visible, false }).has_value(), "sets");
    check(editor.apply(SetTrackFlag{ track_id, TrackFlag::Muted, true }).has_value(), "sets");
    const Track &track = editor.project().active().tracks[0];
    check(!track.visible, "hidden");
    check(track.muted, "muted");

    // Setting the value already in place is a no-op, exactly as the old
    // deep-compare judged it.
    const auto outcome = editor.apply(SetTrackFlag{ track_id, TrackFlag::Muted, true });
    check(outcome.has_value(), "tolerated");
    check(!outcome->applied, "setting the same value is a no-op");
}

void test_rotation_wraps_into_the_half_open_degree_range()
{
    // (-180, 180]: a knob dragged through full turns must not carry the
    // turns with it, and the boundary itself belongs to +180.
    auto [editor, media_id, clip_id] = fixture();
    (void)media_id;
    const double sent[] = { 361.0, -1.0, 720.0, 180.0, -180.0, 540.0 };
    const double landed[] = { 1.0, -1.0, 0.0, 180.0, 180.0, 180.0 };
    for (std::size_t i = 0; i < 6; ++i) {
        const auto set = editor.apply(
                Command{ SetClipTransform{ clip_id, std::nullopt, std::nullopt, std::nullopt,
                                           sent[i], std::nullopt, std::nullopt } });
        check(set.has_value(), "sets");
        const Clip *clip = editor.project().active().clip(clip_id);
        check(clip != nullptr, "exists");
        check_near(clip != nullptr ? clip->rotation : 0.0, landed[i], 1e-12,
                   std::to_string(sent[i]) + " degrees should land at "
                           + std::to_string(landed[i]));
    }
}

void test_removing_a_timeline_moves_the_active_tab_to_a_neighbour()
{
    Editor editor;
    const auto add_second = editor.apply(AddTimeline{ });
    check(add_second.has_value(), "adds");
    check(add_second->created_id.has_value(), "id");
    const std::string second = add_second->created_id.value_or("");
    const auto add_third = editor.apply(AddTimeline{ });
    check(add_third.has_value(), "adds");
    check(add_third->created_id.has_value(), "id");
    const std::string third = add_third->created_id.value_or("");
    check(editor.project().active_timeline_id == third, "the newest is active");

    // An unknown id is a tolerated no-op while the floor allows it.
    const auto outcome = editor.apply(RemoveTimeline{ "ghost" });
    check(outcome.has_value(), "tolerated");
    check(!outcome->applied, "an unknown id is a no-op");

    // Removing the active last tab falls back to the previous one.
    check(editor.apply(RemoveTimeline{ third }).has_value(), "removes");
    check(editor.project().active_timeline_id == second,
          "removing the active last tab falls back to the previous one");

    // Removing an active middle tab prefers the neighbour to its right.
    const auto add_again = editor.apply(AddTimeline{ });
    check(add_again.has_value(), "adds");
    check(add_again->created_id.has_value(), "id");
    const std::string third_again = add_again->created_id.value_or("");
    check(editor.apply(SelectTimeline{ second }).has_value(), "selects");
    check(editor.apply(RemoveTimeline{ second }).has_value(), "removes");
    check(editor.project().active_timeline_id == third_again,
          "removing an active middle tab prefers the neighbour to its right");

    // Removing an inactive tab leaves the selection alone.
    check(editor.apply(RemoveTimeline{ "TL1" }).has_value(), "removes");
    check(editor.project().active_timeline_id == third_again,
          "removing an inactive tab leaves the selection alone");
}

// --- templates, fonts, colour range and missing media -------------------------

// A style without a box height is the words' own, and a hand-edited one
// cannot make it negative or not a number.
// https://github.com/jub0t/Concat/issues/119
void test_a_text_style_without_a_box_height_reads_as_the_words_own()
{
    check(TextStyle{ }.max_height == 0.0, "no height means the words' own");
    TextStyle odd;
    odd.max_height = -3.0;
    check(odd.tidy().max_height == 0.0, "a negative height reads as none");
    TextStyle nan;
    nan.max_height = std::numeric_limits<double>::quiet_NaN();
    check(nan.tidy().max_height == 0.0, "a NaN height reads as none");
}

void test_a_slot_fills_in_place_keeping_the_template_timing()
{
    auto [editor, media_id, clip_id] = fixture();
    // Shape the clip the way a template slot looks: trimmed off the front,
    // so it has a real in-point to forget.
    check(editor.apply(trim(clip_id, TrimEdge::Start, Rational{ 2, 1 }, false)).has_value(),
          "trims");
    check(editor.apply(SetMediaPlaceholder{ media_id, true }).has_value(), "marks");
    check(editor.project().media[0].placeholder, "the slot is marked");

    NewMedia photo;
    photo.path = "/photo.jpg";
    photo.name = "photo.jpg";
    photo.kind = MediaKind::Image;
    photo.width = 4000;
    photo.height = 3000;
    photo.has_audio = false;
    check(editor.apply(FillSlot{ media_id, std::move(photo) }).has_value(), "fills");

    const Project &project = editor.project();
    const MediaItem &media = project.media[0];
    check(!media.placeholder, "a filled slot is ordinary media again");
    check(media.id == media_id, "the id survives, so clips keep working");
    check(media.path == "/photo.jpg", "the path is the new file's");

    const Clip &clip = project.active().clips[0];
    check(clip.start == Rational{ 2, 1 }, "slot position is the template's");
    check(clip.duration == Rational{ 8, 1 }, "slot length is the template's");
    check(clip.source_start == Rational::ZERO, "the in-point referred to the old footage");
    check(clip.kind == ClipKind::Image, "the kind follows the new file");
    check(clip.name == "photo.jpg", "the name follows the new file");
}

void test_filling_ordinary_media_is_refused()
{
    auto [editor, media_id, clip_id] = fixture();
    (void)clip_id;
    NewMedia b;
    b.path = "/b.mp4";
    b.name = "b.mp4";
    b.duration = Rational{ 3, 1 };
    b.kind = MediaKind::Video;
    b.has_audio = false;
    const auto refused = editor.apply(FillSlot{ media_id, std::move(b) });
    check(!refused.has_value(), "filling ordinary media is refused");
    if (!refused.has_value()) {
        check(refused.error().kind == CommandErrorKind::NotASlot, "refused as not a template slot");
    }
}

void test_fonts_deduplicate_by_path_and_removal_leaves_titles_alone()
{
    Editor editor;
    check(editor.apply(AddFont{ "Inter", "/fonts/inter.ttf" }).has_value(), "adds");
    // Same path again: a re-import must not duplicate the entry.
    const auto duplicate = editor.apply(AddFont{ "Inter Again", "/fonts/inter.ttf" });
    check(duplicate.has_value(), "tolerated");
    check(!duplicate->applied, "a duplicate path is a no-op");
    check(editor.project().fonts.size() == 1, "one font");

    TextStyle style;
    style.font_family = "Inter";
    const auto added = editor.apply(
            AddTextClip{ std::nullopt, false, Rational::ZERO, style, std::nullopt, std::nullopt });
    check(added.has_value(), "adds");
    check(added->created_id.has_value(), "id");
    const std::string clip_id = added->created_id.value_or("");

    check(editor.apply(RemoveFont{ "Inter" }).has_value(), "removes");
    check(editor.project().fonts.empty(), "the font is gone");
    const Clip *clip = editor.project().active().clip(clip_id);
    check(clip != nullptr, "exists");
    check(clip != nullptr && clip->text.has_value() && clip->text->font_family == "Inter",
          "the title keeps the family name so the face can come back");

    // Removing what is already gone is a tolerated no-op.
    const auto gone = editor.apply(RemoveFont{ "Inter" });
    check(gone.has_value(), "tolerated");
    check(!gone->applied, "removing what is gone is a no-op");
}

void test_missing_media_detects_nonexistent_paths()
{
    Project project;
    MediaItem item;
    item.id = "m1";
    item.path = "/tmp/concat-does-not-exist.mp4";
    item.name = "Missing Clip";
    item.duration = Rational{ 10, 1 };
    item.kind = MediaKind::Video;
    item.width = 1920;
    item.height = 1080;
    project.media.push_back(std::move(item));

    const std::vector<MissingMedia> missing = project.missing_media();
    check(missing.size() == 1, "one missing item");
    check(missing.size() == 1 && missing[0].id == "m1", "the id is reported");
    check(missing.size() == 1 && missing[0].name == "Missing Clip", "the name is reported");
    check(missing.size() == 1 && missing[0].path == "/tmp/concat-does-not-exist.mp4",
          "the path is reported");
}

void test_missing_media_ignores_existing_paths()
{
    Project project;
    MediaItem item;
    item.id = "m2";
    item.path = "/tmp";
    item.name = "Existing Clip";
    item.duration = Rational{ 5, 1 };
    item.kind = MediaKind::Video;
    item.width = 1920;
    item.height = 1080;
    project.media.push_back(std::move(item));

    check(project.missing_media().empty(), "an existing path is not missing");
}

void test_a_medias_colour_range_is_an_undoable_edit()
{
    auto [editor, media_id, clip_id] = fixture();
    (void)clip_id;
    check(!editor.project().media[0].color_range.has_value(), "absent reads the file's own tag");

    const auto set = editor.apply(SetMediaColorRange{ media_id, ColorRange::Full });
    check(set.has_value(), "sets");
    check(set->applied, "a change is applied");
    check(editor.project().media[0].color_range.has_value()
                  && *editor.project().media[0].color_range == ColorRange::Full,
          "the range is full");

    // Cleared is back to the tag.
    check(editor.apply(SetMediaColorRange{ media_id, std::nullopt }).has_value(), "clears");
    check(!editor.project().media[0].color_range.has_value(),
          "cleared reads the file's own tag again");
    const auto same = editor.apply(SetMediaColorRange{ media_id, std::nullopt });
    check(same.has_value(), "tolerated");
    check(!same->applied, "a value already held changes nothing");
    const auto gone = editor.apply(SetMediaColorRange{ "m999", ColorRange::Limited });
    check(gone.has_value(), "an unknown item is a no-op");
    check(!gone->applied, "an unknown item changes nothing");
    check(!gone->created_id.has_value(), "an unknown item mints nothing");

    editor.undo();
    check(editor.project().media[0].color_range.has_value()
                  && *editor.project().media[0].color_range == ColorRange::Full,
          "undo of the clear brings the range back");
}

} // namespace

int main()
{
    test_ripple_delete_closes_the_gap_on_its_own_track_only();
    test_a_plain_delete_still_leaves_the_hole();
    test_ripple_delete_of_several_clips_moves_each_survivor_by_what_was_before_it();
    test_ripple_delete_counts_overlapping_spans_once();
    test_ripple_delete_of_a_span_reaching_past_a_survivor_pulls_only_up_to_it();
    test_ripple_delete_leaves_a_clip_stacked_at_the_same_start_alone();
    test_ripple_delete_of_unknown_ids_moves_nothing_and_records_nothing();
    test_ripple_delete_is_one_undo_step_that_puts_everything_back();
    test_ripple_delete_of_everything_on_a_lane_is_just_a_delete();

    test_magnetic_tail_trim_moves_the_lane_behind_it_both_ways();
    test_a_plain_trim_still_leaves_the_lane_alone();
    test_magnetic_head_trim_keeps_the_clip_where_it_was_and_closes_behind_it();
    test_a_plain_head_trim_moves_the_head_and_nothing_else();
    test_magnetic_head_trim_can_reach_back_only_as_far_as_the_source_and_moves_nothing_when_it_cannot();
    test_magnetic_head_trim_restoring_source_pushes_the_lane_back();
    test_magnetic_trim_stops_at_the_minimum_and_the_lane_stays_touching();
    test_magnetic_trim_leaves_a_clip_in_front_of_the_edge_alone();
    test_magnetic_trim_is_one_undo_step_that_puts_the_lane_back();

    test_replace_clip_media_can_move_the_in_point_to_the_copy();
    test_replace_clip_media_points_the_clip_at_the_copy_and_keeps_the_original();
    test_freeze_frame_holds_a_second_and_ripples_the_tail();
    test_freeze_frame_on_a_curved_clip_keeps_the_pieces_continuous();
    test_split_produces_source_continuous_halves_and_merge_rejoins_them();
    test_a_head_trim_cannot_reach_before_the_source_begins();

    test_keys_stay_on_their_instant_through_split_trim_and_merge();
    test_a_split_leaves_the_fade_in_with_the_head_and_the_fade_out_with_the_tail();
    test_effect_keys_stay_on_their_instant_through_a_split();
    test_rearranged_pieces_refuse_to_merge();
    test_a_head_trim_moves_the_in_point_with_speed();
    test_speed_clamps_to_the_engine_range();
    test_set_clip_reverse_flips_playback_direction_and_undoes();
    test_set_clip_pan_clamps_and_undoes();

    test_a_video_whose_detached_sound_was_deleted_can_be_unmuted();
    test_detach_and_reattach_round_trip_the_sound();
    test_detaching_a_two_track_recording_gives_one_sound_clip_per_track();
    test_a_command_carrying_nan_is_refused_whole();
    test_nan_in_transition_or_text_patch_is_refused();
    test_timelines_add_switch_and_delete_with_a_floor_of_one();
    test_a_timeline_moves_to_a_new_slot_without_stealing_the_selection();
    test_undo_and_redo_walk_the_history();
    test_a_failed_command_records_no_history();

    test_a_new_project_has_one_timeline_and_four_lanes();
    test_duplicate_media_paths_are_ignored();
    test_a_gesture_is_one_undo_step();
    test_switching_and_reordering_tabs_is_not_an_edit();
    test_a_ripple_copies_only_the_clips_it_moves();
    test_a_command_leaves_a_tidy_clip_behind();
    test_a_command_copies_only_what_it_writes();
    test_a_batch_is_one_undo_step();
    test_a_batch_imports_and_places_media_in_one_step();
    test_a_failed_batch_changes_nothing();
    test_undo_depth_evicts_the_oldest_snapshot_first();

    test_setting_a_value_already_in_place_records_no_undo_entry();
    test_a_batch_reports_applied_only_when_a_member_changed_something();
    test_moves_to_a_vanished_track_keep_the_time_change_and_drop_the_track_change();
    test_a_multi_move_skips_unknown_clips_and_floors_start_at_zero();
    test_a_tail_trim_stretches_only_the_duration_and_stops_at_the_minimum();
    test_add_at_first_free_takes_the_lowest_empty_lane_or_the_bottom_one();
    test_removing_a_track_takes_its_clips_and_stops_at_the_floor_of_one();
    test_removing_media_sweeps_its_clips_from_every_timeline();
    test_renames_trim_whitespace_and_refuse_to_blank_a_name();
    test_track_flags_flip_independently_and_report_no_ops_honestly();
    test_rotation_wraps_into_the_half_open_degree_range();
    test_removing_a_timeline_moves_the_active_tab_to_a_neighbour();

    test_a_text_style_without_a_box_height_reads_as_the_words_own();
    test_a_slot_fills_in_place_keeping_the_template_timing();
    test_filling_ordinary_media_is_refused();
    test_fonts_deduplicate_by_path_and_removal_leaves_titles_alone();
    test_missing_media_detects_nonexistent_paths();
    test_missing_media_ignores_existing_paths();
    test_a_medias_colour_range_is_an_undoable_edit();

    std::printf("%d checks, %d failures\n", checks, failures);
    return failures == 0 ? 0 : 1;
}
