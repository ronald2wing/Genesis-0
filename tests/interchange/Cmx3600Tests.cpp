// SPDX-License-Identifier: GPL-3.0-or-later
// SPDX-FileCopyrightText: 2026 Genesis-0 contributors
//
// The EDL importer: a CMX-3600 text becomes a settled project, and the exporter
// writes the same record timecodes back out.

#include <optional>
#include <string>

#include "core/ClipTimeline.h"
#include "interchange/Cmx3600.h"
#include "project/Project.h"
#include "project/model/Clip.h"
#include "project/model/Media.h"
#include "project/model/Timeline.h"
#include "project/model/VideoSettings.h"
#include "../support/Checks.h"
#include "Fixtures.h"

namespace {

using genesis::test::check;

namespace project = genesis::project;

using genesis::core::Rational;
using genesis::test::interchange::fixture;
using genesis::test::interchange::seconds;

// A 25 fps timeline, the rate an EDL fixture's timecodes are written at.
project::VideoSettings pal()
{
    project::VideoSettings video;
    video.width = 1920;
    video.height = 1080;
    video.rate_num = 25;
    video.rate_den = 1;
    return video;
}

const project::VideoSettings kPal = pal();

bool near(double left, double right)
{
    return std::abs(left - right) < 1e-9;
}

// Three events: two pictures on V, one sound on A, plus a title, an FCM line
// and a comment. Timecodes are chosen so the record times are exact quarters.
project::Timeline import_fixture(std::string &error)
{
    const std::string edl =
            "\xEF\xBB\xBF"
            "TITLE: Reel One\n"
            "FCM: NON-DROP FRAME\n"
            "\n"
            "* a comment line, ignored\n"
            "001  AX       V     C        00:00:00:00 00:00:01:00 00:00:00:00 00:00:01:00\n"
            "002  BX       V     C        00:00:05:00 00:00:09:00 00:00:01:00 00:00:05:00\n"
            "003  CX       A     C        00:00:00:00 00:00:03:00 00:00:00:00 00:00:03:00\n";
    const std::optional<project::Project> imported =
            genesis::interchange::from_cmx3600(edl, kPal, error);
    check(imported.has_value(), "the EDL imports: " + error);
    if (!imported || imported->timelines.empty()) {
        return { };
    }
    return imported->timelines[0];
}

void test_edl_import()
{
    std::string error;
    const project::Timeline timeline = import_fixture(error);
    if (timeline.clips.empty()) {
        return;
    }

    check(timeline.name == "Reel One", "the TITLE names the timeline");
    check(timeline.id == "TL1", "the timeline is TL1");
    check(timeline.video.rate_num == 25 && timeline.video.rate_den == 1,
          "the timeline keeps the caller's rate");
    check(timeline.tracks.size() == 2, "V and A become two tracks");
    check(timeline.clips.size() == 3, "three events become three clips");

    const project::Clip &first = timeline.clips[0];
    check(first.kind == project::ClipKind::Video, "a V column is a video clip");
    check(first.track_id == "T1", "the picture lands on T1");
    check(near(first.start.as_double(), 0.0), "RECIN 00:00:00:00 is zero");
    check(near(first.duration.as_double(), 1.0), "RECOUT - RECIN is one second");
    check(near(first.source_start.as_double(), 0.0), "SRCIN 00:00:00:00 is zero");
    check(first.name == "AX", "the reel name becomes the clip name");

    const project::Clip &second = timeline.clips[1];
    check(near(second.start.as_double(), 1.0), "RECIN 00:00:01:00 is one second");
    check(near(second.duration.as_double(), 4.0), "00:00:05:00 - 00:00:01:00 is four seconds");
    check(near(second.source_start.as_double(), 5.0), "SRCIN 00:00:05:00 is five seconds");

    const project::Clip &audio = timeline.clips[2];
    check(audio.kind == project::ClipKind::Audio, "an A column is an audio clip");
    check(audio.track_id == "T2", "the sound lands on T2");
    check(near(audio.duration.as_double(), 3.0), "the sound is three seconds");
}

void test_shared_reel_is_one_media_item()
{
    const std::string edl =
            "TITLE: Shared\n"
            "FCM: NON-DROP FRAME\n\n"
            "001  AX       V     C        00:00:00:00 00:00:01:00 00:00:00:00 00:00:01:00\n"
            "002  AX       V     C        00:00:01:00 00:00:02:00 00:00:01:00 00:00:02:00\n";
    std::string error;
    const std::optional<project::Project> imported =
            genesis::interchange::from_cmx3600(edl, kPal, error);
    check(imported.has_value(), "a shared-reel EDL imports: " + error);
    if (!imported) {
        return;
    }
    check(imported->media.size() == 1, "one reel is one media item");
    if (imported->media.empty()) {
        return;
    }
    check(imported->media[0].path == "AX", "the reel name is the media path");
    check(imported->media[0].name == "AX", "the reel name is the media name");
    check(imported->media[0].kind == project::MediaKind::Video, "a picture reel is video media");
    check(imported->timelines[0].clips.size() == 2, "both events still land as clips");
    check(imported->timelines[0].clips[0].media_id == imported->timelines[0].clips[1].media_id,
          "the two clips share the one media item");
}

void test_malformed_edls()
{
    std::string error;

    // Fewer than eight fields.
    const std::optional<project::Project> short_line = genesis::interchange::from_cmx3600(
            "001  AX       V     C        00:00:00:00 00:00:01:00 00:00:00:00\n", kPal, error);
    check(!short_line.has_value(), "a seven-field event is refused");
    check(!error.empty(), "the short line error names a problem");

    // A timecode that does not parse.
    const std::optional<project::Project> bad_timecode = genesis::interchange::from_cmx3600(
            "001  AX       V     C        00:00:00:00 00:00:01:00 xx:00:00:00 "
            "00:00:01:00\n",
            kPal, error);
    check(!bad_timecode.has_value(), "an unreadable timecode is refused");
    check(!error.empty(), "the bad timecode error names a problem");

    // No events at all.
    const std::optional<project::Project> empty = genesis::interchange::from_cmx3600(
            "TITLE: Nothing\nFCM: NON-DROP FRAME\n", kPal, error);
    check(!empty.has_value(), "an EDL with no events is refused");
    check(error == "the file holds no EDL events", "the empty EDL error says why");
}

void test_edl_round_trip()
{
    std::string error;
    const std::string edl =
            "TITLE: Reel One\n"
            "FCM: NON-DROP FRAME\n\n"
            "001  AX       V     C        00:00:00:00 00:00:01:00 00:00:00:00 00:00:01:00\n";
    const std::optional<project::Project> imported =
            genesis::interchange::from_cmx3600(edl, kPal, error);
    check(imported.has_value(), "the round-trip EDL imports: " + error);
    if (!imported) {
        return;
    }
    const std::string written = genesis::interchange::to_cmx3600(*imported);
    check(written.find("00:00:00:00 00:00:01:00 00:00:00:00 00:00:01:00") != std::string::npos,
          "the first event re-exports the original record timecodes");
}

// A project with one picture at 0..1s and one sound at 1..4s, so the exporter
// has one of each kind to write.
project::Project picture_and_sound()
{
    project::Project project(kPal);

    project::MediaItem picture;
    picture.id = "m1";
    picture.name = "AX";
    picture.path = "AX";
    picture.kind = project::MediaKind::Video;
    project.media.push_back(std::move(picture));

    project::MediaItem sound;
    sound.id = "m2";
    sound.name = "BX";
    sound.path = "BX";
    sound.kind = project::MediaKind::Audio;
    project.media.push_back(std::move(sound));

    project::Clip video = project::Clip::blank("c1", "T1", project::ClipKind::Video, "AX",
                                               Rational::ZERO, Rational::from_int(1));
    video.media_id = "m1";
    video.source_start = Rational::ZERO;

    project::Clip audio = project::Clip::blank("c2", "T2", project::ClipKind::Audio, "BX",
                                               Rational::from_int(1), Rational::from_int(3));
    audio.media_id = "m2";
    audio.source_start = Rational::from_int(2);

    project.timelines[0].clips.push_back(std::move(video));
    project.timelines[0].clips.push_back(std::move(audio));
    return project;
}

void test_audio_round_trip()
{
    const project::Project project = picture_and_sound();
    const std::string written = genesis::interchange::to_cmx3600(project);

    // Two events: the picture on V, the sound on A, each with its four
    // timecodes. The reel column is the exporter's fixed "AX"; the track
    // column is what tells a picture from a sound.
    check(written.find("001  AX       V     C        00:00:00:00 00:00:01:00 "
                       "00:00:00:00 00:00:01:00")
                  != std::string::npos,
          "the video clip is a V event with its timecodes");
    check(written.find("002  AX       A     C        00:00:02:00 00:00:05:00 "
                       "00:00:01:00 00:00:04:00")
                  != std::string::npos,
          "the audio clip is an A event with its timecodes");

    // Re-importing the exported file gives both clips back with their kinds
    // and times intact.
    std::string error;
    const std::optional<project::Project> imported =
            genesis::interchange::from_cmx3600(written, kPal, error);
    check(imported.has_value(), "the audio round-trip re-imports: " + error);
    if (!imported || imported->timelines.empty()) {
        return;
    }
    const project::Timeline &timeline = imported->timelines[0];
    check(timeline.clips.size() == 2, "the round trip keeps both clips");

    const project::Clip &video = timeline.clips[0];
    check(video.kind == project::ClipKind::Video, "the picture comes back as video");
    check(video.track_id == "T1", "the picture is back on T1");
    check(near(video.start.as_double(), 0.0), "the picture record-in is zero");
    check(near(video.duration.as_double(), 1.0), "the picture is one second");

    const project::Clip &audio = timeline.clips[1];
    check(audio.kind == project::ClipKind::Audio, "the sound comes back as audio");
    check(audio.track_id == "T2", "the sound is back on T2");
    check(near(audio.start.as_double(), 1.0), "the sound record-in is one second");
    check(near(audio.duration.as_double(), 3.0), "the sound is three seconds");
    check(near(audio.source_start.as_double(), 2.0), "the sound source-in is two seconds");
}

void test_audio_before_video_numbering()
{
    // The sound starts at 0s, before the picture at 2s: the union sorted by
    // record in must number the sound first so the file reads chronologically.
    project::Project project(kPal);
    project::Clip audio = project::Clip::blank("c1", "T2", project::ClipKind::Audio, "BX",
                                               Rational::ZERO, Rational::from_int(1));
    audio.media_id = "m1";
    project::Clip video = project::Clip::blank("c2", "T1", project::ClipKind::Video, "AX",
                                               Rational::from_int(2), Rational::from_int(1));
    video.media_id = "m2";
    project.timelines[0].clips.push_back(std::move(audio));
    project.timelines[0].clips.push_back(std::move(video));

    const std::string written = genesis::interchange::to_cmx3600(project);
    check(written.find("001  AX       A     C        ") != std::string::npos,
          "the earlier audio event is numbered 001 on the A column");
    check(written.find("002  AX       V     C        ") != std::string::npos,
          "the later video event is numbered 002 on the V column");
}

// A dissolve column carries its duration before the timecodes, and it is the
// clip's cut-in, so it must land as a transition rather than be skipped.
void test_dissolve_column()
{
    const std::string edl = "TITLE: Dissolve\n"
                            "FCM: NON-DROP FRAME\n"
                            "\n"
                            "001  AX       V     C        00:00:00:00 00:00:01:00 "
                            "00:00:00:00 00:00:01:00\n"
                            "002  AX       V     D    024 00:00:02:00 00:00:03:00 "
                            "00:00:01:00 00:00:02:00\n";
    std::string error;
    const std::optional<project::Project> imported =
            genesis::interchange::from_cmx3600(edl, kPal, error);
    check(imported.has_value(), "a dissolve EDL imports: " + error);
    if (!imported || imported->timelines.empty()) {
        return;
    }
    const project::Timeline &timeline = imported->timelines.front();
    check(timeline.clips.size() == 2, "both events import");
    if (timeline.clips.size() != 2) {
        return;
    }
    check(!timeline.clips[0].transition_in.has_value(), "a cut has no transition");
    const project::Clip &dissolve = timeline.clips[1];
    check(dissolve.transition_in.has_value(), "the dissolve becomes a transition-in");
    if (dissolve.transition_in) {
        check(dissolve.transition_in->id == "cross-fade", "it names the cross-fade");
        check(dissolve.transition_in->duration == Rational(24, 25),
              "its 24-frame duration is exact at 25 fps");
    }
    check(near(dissolve.start.as_double(), 1.0), "the shifted timecode columns still read");
    check(near(dissolve.source_start.as_double(), 2.0), "including the source in-point");
}

// Drop-frame numbering skips two labels a minute (bar every tenth), so the same
// printed timecode is two frames earlier than a non-drop reading of it.
void test_drop_frame_timecode()
{
    project::VideoSettings ntsc;
    ntsc.width = 1920;
    ntsc.height = 1080;
    ntsc.rate_num = 30000;
    ntsc.rate_den = 1001;

    const std::string edl = "TITLE: Drop\n"
                            "FCM: DROP FRAME\n"
                            "\n"
                            "001  AX       V     C        00:00:00:00 00:00:01:00 "
                            "00:01:00:02 00:01:01:02\n";
    std::string error;
    const std::optional<project::Project> imported =
            genesis::interchange::from_cmx3600(edl, ntsc, error);
    check(imported.has_value(), "a drop-frame EDL imports: " + error);
    if (!imported || imported->timelines.empty() || imported->timelines[0].clips.empty()) {
        return;
    }
    // 00:01:00:02 is label 1802; drop-frame removes the two skipped labels, so
    // the real frame is 1800, which at 30000/1001 is 60.06 s.
    const project::Clip &clip = imported->timelines[0].clips.front();
    check(near(clip.start.as_double(), 1800.0 * 1001.0 / 30000.0),
          "the dropped labels are removed from the frame count");
}

// One reel on both the V and A columns is two assets: pointing the sound at the
// picture file would corrupt the A/V association on relink.
void test_reel_on_both_columns_is_two_media()
{
    const std::string edl = "TITLE: Shared\n"
                            "FCM: NON-DROP FRAME\n"
                            "\n"
                            "001  REEL1    V     C        00:00:00:00 00:00:01:00 "
                            "00:00:00:00 00:00:01:00\n"
                            "002  REEL1    A     C        00:00:00:00 00:00:01:00 "
                            "00:00:00:00 00:00:01:00\n";
    std::string error;
    const std::optional<project::Project> imported =
            genesis::interchange::from_cmx3600(edl, kPal, error);
    check(imported.has_value(), "a shared-reel EDL imports: " + error);
    if (!imported) {
        return;
    }
    check(imported->media.size() == 2, "the reel becomes one picture and one sound asset");
    if (imported->media.size() == 2) {
        check(imported->media[0].kind == project::MediaKind::Video
                      && imported->media[1].kind == project::MediaKind::Audio,
              "each carries its own kind");
    }
}

} // namespace

void test_edl_text()
{
    const project::Project edited = fixture();
    const std::string text = genesis::interchange::to_cmx3600(edited);
    check(text.rfind("TITLE: Cut\n", 0) == 0, "the EDL starts with the timeline title");
    check(text.find("FCM: NON-DROP FRAME") != std::string::npos, "the EDL declares non-drop frame");
    check(text.find("001  AX       V     C        "
                    "00:00:00:00 00:00:01:00 00:00:00:00 00:00:01:00")
                  != std::string::npos,
          "the first event carries source and record timecodes");
    // Events are numbered in record order, so the sound that also starts at
    // zero follows the picture that starts there.
    check(text.find("002  AX       A     C        "
                    "00:00:00:00 00:00:03:00 00:00:00:00 00:00:03:00")
                  != std::string::npos,
          "audio is an A-column event at its own record position");
    check(text.find("003  AX       V     C        "
                    "00:00:05:00 00:00:07:00 00:00:01:00 00:00:03:00")
                  != std::string::npos,
          "the second picture keeps its in-point and record position");
}

int main()
{
    test_edl_import();
    test_shared_reel_is_one_media_item();
    test_malformed_edls();
    test_edl_round_trip();
    test_audio_round_trip();
    test_audio_before_video_numbering();
    test_dissolve_column();
    test_drop_frame_timecode();
    test_reel_on_both_columns_is_two_media();
    test_edl_text();
    return genesis::test::summary();
}
