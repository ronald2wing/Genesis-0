// SPDX-License-Identifier: GPL-3.0-or-later
// SPDX-FileCopyrightText: 2026 Genesis-0 contributors
//
// The project the format suites share: two pictures on T1 and one sound on T2,
// with both kinds of media in the bin. It lives here rather than in one suite
// because the export tests for all three formats need the same shape, and a
// copy per suite drifts the moment a field is added.

#pragma once

#include "core/ClipTimeline.h"
#include "project/Project.h"
#include "project/model/Clip.h"
#include "project/model/Media.h"
#include "project/model/Timeline.h"

namespace genesis::test::interchange {

inline genesis::core::Rational seconds(double value)
{
    return genesis::core::Rational::approximate(value).value_or(genesis::core::Rational::ZERO);
}

inline genesis::project::Project fixture()
{
    using namespace genesis::project;

    Project edited;
    edited.timelines[0].name = "Cut";
    edited.timelines[0].video.width = 1920;
    edited.timelines[0].video.height = 1080;
    edited.timelines[0].video.rate_num = 30;
    edited.timelines[0].video.rate_den = 1;

    MediaItem picture;
    picture.id = "m1";
    picture.path = "/media/a.mp4";
    picture.name = "a.mp4";
    picture.kind = MediaKind::Video;
    edited.media.push_back(picture);

    MediaItem sound;
    sound.id = "m2";
    sound.path = "/media/b.wav";
    sound.name = "b.wav";
    sound.kind = MediaKind::Audio;
    edited.media.push_back(sound);

    Clip first;
    first.id = "c1";
    first.track_id = "T1";
    first.media_id = "m1";
    first.name = "One";
    first.kind = ClipKind::Video;
    first.start = seconds(0.0);
    first.duration = seconds(1.0);
    first.source_start = seconds(0.0);
    edited.timelines[0].clips.push_back(first);

    Clip second = first;
    second.id = "c2";
    second.name = "Two";
    second.start = seconds(1.0);
    second.duration = seconds(2.0);
    second.source_start = seconds(5.0);
    edited.timelines[0].clips.push_back(second);

    Clip audio = first;
    audio.id = "c3";
    audio.track_id = "T2";
    audio.media_id = "m2";
    audio.name = "Sound";
    audio.kind = ClipKind::Audio;
    audio.start = seconds(0.0);
    audio.duration = seconds(3.0);
    audio.source_start = seconds(0.0);
    edited.timelines[0].clips.push_back(audio);

    return edited;
}

} // namespace genesis::test::interchange
