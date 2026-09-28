// SPDX-License-Identifier: GPL-3.0-or-later
// SPDX-FileCopyrightText: 2026 Genesis-0 contributors
//
// Ported from Concat's concat-project document tests (lib.rs),
// SPDX-License-Identifier: GPL-3.0-or-later,
// SPDX-FileCopyrightText: 2026 Jareer and Concat contributors.

#include <algorithm>
#include <array>
#include <cmath>
#include <cstddef>
#include <cstdio>
#include <optional>
#include <string>
#include <vector>

#include <nlohmann/json.hpp>

#include "project/Project.h"
#include "project/command/ClipCommands.h"
#include "project/command/Command.h"
#include "project/command/MediaCommands.h"
#include "project/command/PropertyCommands.h"
#include "project/command/TimelineCommands.h"
#include "project/model/Clip.h"
#include "project/model/Cutout.h"
#include "project/model/Effects.h"
#include "project/model/Keyframe.h"
#include "project/model/Media.h"
#include "project/model/Speed.h"
#include "project/model/Text.h"
#include "project/model/Timeline.h"
#include "project/model/Track.h"
#include "project/model/VideoSettings.h"
#include "project/Document.h"
#include "project/Editor.h"
#include "project/IdMinter.h"

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

// --- test helpers -----------------------------------------------------------

DocumentSettings settings()
{
    DocumentSettings settings;
    settings.name = "Test";
    return settings;
}

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

// --- document version -------------------------------------------------------

void test_a_document_from_a_newer_build_is_refused()
{
    auto [editor, media_id, clip_id] = fixture();
    (void)media_id;
    (void)clip_id;
    nlohmann::json document = to_document(editor.project(), settings());
    document["version"] = DOCUMENT_VERSION + 1;
    check(!from_document(document).has_value(), "a document from a newer build is refused");
    const std::optional<int> version = document_version(document);
    check(version.has_value() && *version == DOCUMENT_VERSION + 1, "the newer version is reported");
    // The Rust `document_version` defaults a missing field to 1; the host's
    // returns the absent value, and the loader itself treats absent as the
    // oldest, so a document without a version still opens.
    check(!document_version(nlohmann::json::object()).has_value(),
          "a missing version reads as the absent value");
}

// --- timeline frame migration -----------------------------------------------

void test_a_timeline_without_its_own_frame_takes_the_documents()
{
    const nlohmann::json document = nlohmann::json::parse(R"({
        "name": "Old", "version": 1,
        "video": { "width": 1080, "height": 1920, "rateNum": 60, "rateDen": 1 },
        "media": [],
        "timelines": [
            { "id": "TL1", "name": "Vertical",
              "tracks": [{ "id": "T1", "visible": true, "muted": false }], "clips": [] },
            { "id": "TL2", "name": "Half", "video": { "width": 0, "height": 540 },
              "tracks": [{ "id": "T2", "visible": true, "muted": false }], "clips": [] }
        ],
        "activeTimelineId": "TL2"
    })");
    const std::optional<Project> loaded = from_document(document);
    check(loaded.has_value(), "loads");
    if (!loaded.has_value()) {
        return;
    }
    const std::vector<Timeline> &timelines = loaded->timelines;
    check(timelines.size() == 2 && timelines[0].video.width == 1080
                  && timelines[0].video.height == 1920,
          "a timeline without its own frame takes the document's");
    check(timelines.size() == 2 && timelines[0].video.rate_num == 60, "and its rate");
    check(timelines.size() == 2 && timelines[1].video.width == 1080
                  && timelines[1].video.height == 540,
          "a zero width takes the document's; a stated height is kept");
    check(loaded->active_timeline_id == "TL2", "the active timeline survives");
}

// --- tolerant reading -------------------------------------------------------

void test_an_entry_the_reader_cannot_parse_is_dropped_not_the_document()
{
    const nlohmann::json document = nlohmann::json::parse(R"({
        "name": "Mixed", "version": 1,
        "media": [
            { "id": "m1", "path": "/a.mp4", "kind": "hologram" },
            { "id": "m2", "path": "/b.mp4", "kind": "audio", "audioTracks": "nope" },
            { "path": "/no-id.mp4" }
        ],
        "tracks": [{ "id": "T1" }, { "visible": false }],
        "clips": [
            { "id": "c1", "trackId": "T1", "mediaId": "m1", "kind": "video",
              "cutout": { "mode": "unknown" }, "keys": "garbage",
              "transitionIn": { "id": "cross-fade" },
              "videoEffects": [{ "id": "sepia", "keys": { "amount": [ { "at": 0.5, "value": 1.0 }, { "at": 7.0, "value": 2.0 } ] } }, "not an effect"] },
            { "id": "c2", "trackId": "T1", "mediaId": "m2", "kind": "audio", "start": "soon" }
        ]
    })");
    const std::optional<Project> loaded = from_document(document);
    check(loaded.has_value(), "loads");
    if (!loaded.has_value()) {
        return;
    }
    const Project &project = *loaded;
    check(project.media.size() == 2, "the entry without an id is dropped");
    check(project.media.size() == 2 && project.media[0].kind == MediaKind::Video,
          "an unknown kind is video");
    check(project.media.size() == 2 && project.media[0].name == "/a.mp4",
          "a nameless entry is called by its path");
    check(project.media.size() == 2 && project.media[1].audio_tracks.empty(),
          "a list that is not one is empty");
    const Timeline &timeline = project.active();
    check(timeline.tracks.size() == 1, "the lane without an id is dropped");
    std::vector<std::string> ids;
    for (const Clip &clip : timeline.clips) {
        ids.push_back(clip.id);
    }
    check(ids == std::vector<std::string>{ "c1" }, "a clip whose start is not a number is dropped");
    const Clip *clip = timeline.clip("c1");
    check(clip != nullptr, "c1 is kept");
    if (clip != nullptr) {
        check(!clip->cutout.has_value(), "a cutout that does not parse is no cutout");
        check(clip->keys.empty(), "garbage keys are dropped");
        check(clip->transition_in.has_value() && clip->transition_in->duration == Rational::ONE,
              "a transition without a duration is one second");
        check(clip->video_effects.size() == 1, "a non-effect entry is dropped");
        check(clip->video_effects.size() == 1
                      && clip->video_effects[0].keys_on("amount").size() == 1,
              "the out-of-range key is dropped");
    }
}

// --- round trip -------------------------------------------------------------

void test_the_document_round_trips()
{
    auto [editor, media_id, clip_id] = fixture();
    (void)media_id;
    ClipPatch patch;
    patch.volume = 0.5;
    patch.transition_in = std::optional<Transition>{ Transition{ "cross-fade", Rational{ 3, 2 } } };
    check(editor.apply(UpdateClip{ clip_id, std::move(patch) }).has_value(), "updates");
    check(editor.apply(AddTimeline{ }).has_value(), "adds");

    const nlohmann::json document = to_document(editor.project(), settings());
    const std::optional<Project> restored = from_document(document);
    check(restored.has_value(), "loads");
    check(restored.has_value() && *restored == editor.project(), "the document round-trips");
}

// --- reverse flag ------------------------------------------------------------

// A reversed clip writes its flag, keeps it through a save and an open, and a
// document from a build before the flag reads as forwards.
void test_the_reverse_flag_round_trips_and_defaults_off()
{
    auto [editor, media_id, clip_id] = fixture();
    (void)media_id;
    check(editor.apply(SetClipReverse{ clip_id, true }).has_value(), "reverses");
    const nlohmann::json document = to_document(editor.project(), settings());
    check(document["timelines"][0]["clips"][0]["reverse"] == true, "the flag writes");
    const std::optional<Project> restored = from_document(document);
    check(restored.has_value() && !restored->active().clips.empty()
                  && restored->active().clips[0].reverse,
          "the flag survives a save and an open");

    // A document without the field still opens, forwards.
    const nlohmann::json legacy = nlohmann::json::parse(R"({
        "name": "Old", "version": 1,
        "media": [{ "id": "m1", "path": "/a.mp4", "name": "a.mp4", "kind": "video",
                    "hasAudio": false }],
        "tracks": [{ "id": "T1", "name": "Track 1", "visible": true, "muted": false }],
        "clips": [{ "id": "c1", "trackId": "T1", "mediaId": "m1", "name": "a.mp4",
                    "kind": "video", "start": 0.0, "duration": 10.0,
                    "sourceStart": 0.0 }]
    })");
    const std::optional<Project> old = from_document(legacy);
    check(old.has_value() && !old->active().clips.empty() && !old->active().clips[0].reverse,
          "a document before reverse defaults the flag off");
}

// --- pan --------------------------------------------------------------------

// A clip's pan writes when off-centre, is omitted at centre, and a document
// from a build before the field reads as centred.
void test_the_pan_round_trips_and_defaults_centred()
{
    auto [editor, media_id, clip_id] = fixture();
    (void)media_id;

    const nlohmann::json centred = to_document(editor.project(), settings());
    check(centred["timelines"][0]["clips"][0].find("pan")
                  == centred["timelines"][0]["clips"][0].end(),
          "a centred pan is the absent value");

    check(editor.apply(SetClipPan{ clip_id, -0.5 }).has_value(), "pans");
    const nlohmann::json document = to_document(editor.project(), settings());
    check(document["timelines"][0]["clips"][0]["pan"] == -0.5, "the pan writes");
    const std::optional<Project> restored = from_document(document);
    check(restored.has_value() && !restored->active().clips.empty()
                  && restored->active().clips[0].pan == -0.5,
          "the pan survives a save and an open");

    const nlohmann::json legacy = nlohmann::json::parse(R"({
        "name": "Old", "version": 1,
        "media": [{ "id": "m1", "path": "/a.mp4", "name": "a.mp4", "kind": "video",
                    "hasAudio": false }],
        "tracks": [{ "id": "T1", "name": "Track 1", "visible": true, "muted": false }],
        "clips": [{ "id": "c1", "trackId": "T1", "mediaId": "m1", "name": "a.mp4",
                    "kind": "video", "start": 0.0, "duration": 10.0,
                    "sourceStart": 0.0 }]
    })");
    const std::optional<Project> old = from_document(legacy);
    check(old.has_value() && !old->active().clips.empty() && old->active().clips[0].pan == 0.0,
          "a document before pan defaults the clip centred");
}

// --- legacy documents -------------------------------------------------------

void test_a_version_one_document_loads()
{
    // The shape of a version-1 document as it sits on disk, optional fields
    // omitted - documents like this exist and must load forever.
    const nlohmann::json document = nlohmann::json::parse(R"({
        "concat": "0.1.0", "version": 1, "name": "v1",
        "video": { "width": 1920, "height": 1080, "rateNum": 30, "rateDen": 1 },
        "media": [{ "id": "m1", "path": "/a.mp4", "name": "a.mp4", "duration": 10.0,
                    "kind": "video", "width": 1920, "height": 1080, "frameRate": 30.0,
                    "frameRateFraction": "30/1", "videoCodec": "h264",
                    "audioCodec": null, "hasAudio": false }],
        "tracks": [{ "id": "T1", "name": "Track 1", "visible": true, "muted": false }],
        "clips": [{ "id": "c1", "trackId": "T1", "mediaId": "m1", "name": "a.mp4",
                    "kind": "video", "start": 0.0, "duration": 10.0, "sourceStart": 0.0,
                    "volume": 1.0, "fadeIn": 0.0, "fadeOut": 0.0, "scale": 1.0,
                    "offsetX": 0.0, "offsetY": 0.0, "rotation": 0.0, "opacity": 1.0,
                    "speed": 1.0, "preservePitch": true, "filters": [],
                    "videoEffects": [{ "id": "sepia", "params": {}, "enabled": true }] }],
        "fonts": [],
        "timelines": [{ "id": "TL1", "name": "Timeline 1",
            "tracks": [{ "id": "T1", "name": "Track 1", "visible": true, "muted": false }],
            "clips": [{ "id": "c1", "trackId": "T1", "mediaId": "m1", "name": "a.mp4",
                        "kind": "video", "start": 0.0, "duration": 10.0, "sourceStart": 0.0,
                        "volume": 1.0, "fadeIn": 0.0, "fadeOut": 0.0, "scale": 1.0,
                        "offsetX": 0.0, "offsetY": 0.0, "rotation": 0.0, "opacity": 1.0,
                        "speed": 1.0, "preservePitch": true, "filters": [],
                        "videoEffects": [{ "id": "sepia", "params": {}, "enabled": true }] }] }],
        "activeTimelineId": "TL1"
    })");
    const std::optional<Project> loaded = from_document(document);
    check(loaded.has_value(), "loads");
    check(loaded.has_value() && loaded->timelines.size() == 1, "one timeline");
    check(loaded.has_value() && !loaded->active().clips.empty()
                  && loaded->active().clips[0].video_effects.size() == 1
                  && loaded->active().clips[0].video_effects[0].id == "sepia",
          "the effect id survives");
}

void test_a_legacy_flat_document_loads_as_one_timeline()
{
    const nlohmann::json document = nlohmann::json::parse(R"({
        "name": "Old", "version": 1,
        "media": [],
        "tracks": [{ "id": "T1", "name": "Track 1", "visible": true, "muted": false }],
        "clips": []
    })");
    const std::optional<Project> loaded = from_document(document);
    check(loaded.has_value(), "loads");
    check(loaded.has_value() && loaded->timelines.size() == 1, "one timeline");
    check(loaded.has_value() && loaded->timelines[0].name == "Timeline 1",
          "named like the founding timeline");
}

// --- garbage and damage -----------------------------------------------------

void test_a_garbage_document_is_none_not_a_panic()
{
    check(!from_document(nlohmann::json::parse(R"("nonsense")")).has_value(),
          "a non-object document is refused");
    check(!from_document(nlohmann::json::parse(R"({ "tracks": [] })")).has_value(),
          "a document with no timeline is refused");
}

void test_clips_for_vanished_tracks_or_media_are_dropped_on_load()
{
    // A hand-edited or truncated file must degrade to something openable: a
    // clip with nowhere to live (or nothing to show) is dropped, while a text
    // clip - which needs no media - survives.
    const nlohmann::json document = nlohmann::json::parse(R"({
        "name": "Damaged", "version": 1,
        "media": [{ "id": "m1", "path": "/a.mp4", "name": "a.mp4", "kind": "video",
                    "hasAudio": false }],
        "tracks": [{ "id": "T1", "name": "Track 1", "visible": true, "muted": false }],
        "clips": [
            { "id": "c1", "trackId": "ghost", "mediaId": "m1", "kind": "video",
              "start": 0.0, "duration": 1.0 },
            { "id": "c2", "trackId": "T1", "mediaId": "ghost", "kind": "video",
              "start": 0.0, "duration": 1.0 },
            { "id": "c3", "trackId": "T1", "mediaId": "m1", "kind": "video",
              "start": 0.0, "duration": 1.0 },
            { "id": "c4", "trackId": "T1", "kind": "text", "start": 0.0, "duration": 1.0 }
        ]
    })");
    const std::optional<Project> loaded = from_document(document);
    check(loaded.has_value(), "loads");
    if (!loaded.has_value()) {
        return;
    }
    std::vector<std::string> ids;
    for (const Clip &clip : loaded->active().clips) {
        ids.push_back(clip.id);
    }
    check(ids == std::vector<std::string>{ "c3", "c4" },
          "clips with nowhere to live or nothing to show are dropped");
}

// --- clamped on load --------------------------------------------------------

void test_hand_edited_values_are_clamped_on_load()
{
    // The reader trusts nothing: values that would render invisibly or fall
    // outside the engine's ranges are pulled back in, not obeyed.
    const nlohmann::json document = nlohmann::json::parse(R"({
        "name": "Edited", "version": 1,
        "media": [{ "id": "m1", "path": "/a.mp4", "name": "a.mp4", "kind": "video",
                    "hasAudio": false }],
        "tracks": [{ "id": "T1", "name": "Track 1", "visible": true, "muted": false }],
        "clips": [
            { "id": "c1", "trackId": "T1", "mediaId": "m1", "kind": "video",
              "start": -4.0, "duration": -3.0, "sourceStart": -1.0,
              "volume": -2.0, "opacity": 2.0, "speed": 100.0, "scale": 0.0,
              "pan": 5.0,
              "transitionIn": { "id": "cross-fade", "duration": 0.0 } },
            { "id": "c2", "trackId": "T1", "kind": "text", "start": 0.0, "duration": 1.0,
              "text": { "content": "Hi", "fontSize": 0.0, "fontWeight": 9999.0,
                        "lineHeight": 0.1, "opacity": 5.0 } }
        ]
    })");
    const std::optional<Project> loaded = from_document(document);
    check(loaded.has_value(), "loads");
    if (!loaded.has_value()) {
        return;
    }
    const Timeline &timeline = loaded->active();
    check(timeline.clips.size() == 2, "two clips");
    if (timeline.clips.size() < 2) {
        return;
    }
    const Clip &clip = timeline.clips[0];
    check(clip.start == Rational::ZERO, "a negative start clamps to zero");
    check(clip.duration == Rational{ 1, 60 }, "the floor every command holds");
    check(clip.source_start == Rational::ZERO, "a negative in-point clamps to zero");
    check(clip.volume == 0.0, "a negative volume clamps to zero");
    check(clip.pan == 1.0, "a pan past one clamps to it");
    check(clip.opacity == 1.0, "an opacity past one clamps to one");
    check(clip.speed == 16.0, "a speed past the ceiling clamps to it");
    check(clip.scale == 0.05, "a zero scale clamps to the floor");
    check(clip.transition_in.has_value() && clip.transition_in->duration == Rational{ 1, 10 },
          "a zero transition clamps to a tenth");

    const std::optional<TextStyle> &text = timeline.clips[1].text;
    check(text.has_value(), "the text survives");
    if (text.has_value()) {
        check(text->font_size == 0.01, "a zero size would render an invisible title");
        check(text->font_weight == 900.0, "a weight past nine hundred clamps");
        check(text->line_height == 0.5, "lines cannot collapse onto each other");
        check(text->opacity == 1.0, "an opacity past one clamps");
    }
}

// --- crop wire semantics ----------------------------------------------------

// A null crop on the wire takes the crop off: the patch's double-optional
// keeps "clear it" and "leave it alone" distinct, so a caller across the API
// can still remove a crop.
void test_a_null_crop_on_the_wire_takes_the_crop_off()
{
    auto [editor, media_id, clip_id] = fixture();
    (void)media_id;

    ClipPatch crop_patch;
    crop_patch.crop = std::optional<Crop>{ Crop{ 0.2, 0.0, 0.0, 0.1 } };
    check(editor.apply(UpdateClip{ clip_id, std::move(crop_patch) }).has_value(), "crops");
    check(editor.project().active().clips[0].crop.has_value(), "the crop is on");

    ClipPatch null_patch;
    null_patch.crop = std::optional<Crop>{ };
    const auto uncrop = editor.apply(UpdateClip{ clip_id, std::move(null_patch) });
    check(uncrop.has_value() && uncrop->applied, "null is take it off, not leave it");
    check(!editor.project().active().clips[0].crop.has_value(), "the crop is off");

    ClipPatch untouched;
    untouched.opacity = 0.5;
    check(!untouched.crop.has_value(), "an untouched patch leaves the crop alone");
}

// --- colour space -----------------------------------------------------------

// A timeline's colour is left out of the document for SDR, kept through a
// save and an open for HDR, and read as SDR from a document naming one this
// build does not know; a file's likewise.
void test_a_colour_space_round_trips()
{
    auto [editor, media_id, clip_id] = fixture();
    (void)media_id;
    (void)clip_id;

    const nlohmann::json sdr_doc = to_document(editor.project(), settings());
    check(sdr_doc["timelines"][0]["video"].find("colorSpace")
                  == sdr_doc["timelines"][0]["video"].end(),
          "SDR documents unchanged");

    const std::string timeline_id = editor.project().active_timeline_id;
    VideoSettings hlg = editor.project().active().video;
    hlg.color_space = ColorSpace::Hlg;
    check(editor.apply(SetTimelineVideo{ timeline_id, hlg }).has_value(), "sets");
    const nlohmann::json hlg_doc = to_document(editor.project(), settings());
    check(hlg_doc["timelines"][0]["video"]["colorSpace"] == "hlg", "HLG writes as its word");
    const std::optional<Project> hlg_loaded = from_document(hlg_doc);
    check(hlg_loaded.has_value() && hlg_loaded->active().video.color_space == ColorSpace::Hlg,
          "HLG survives a save and an open");

    const nlohmann::json unknown = nlohmann::json::parse(R"({
        "name": "Unknown", "version": 1,
        "media": [],
        "timelines": [
            { "id": "TL1", "name": "Timeline 1",
              "video": { "width": 1920, "height": 1080, "rateNum": 30, "rateDen": 1,
                         "colorSpace": "dolbyVision" },
              "tracks": [{ "id": "T1", "name": "Track 1", "visible": true, "muted": false }],
              "clips": [] }
        ],
        "activeTimelineId": "TL1"
    })");
    const std::optional<Project> unknown_loaded = from_document(unknown);
    check(unknown_loaded.has_value()
                  && unknown_loaded->active().video.color_space == ColorSpace::Sdr,
          "a colour this build does not know reads as SDR");

    NewMedia hdr;
    hdr.path = "/hdr10.mkv";
    hdr.name = "hdr10.mkv";
    hdr.duration = Rational{ 5, 1 };
    hdr.kind = MediaKind::Video;
    hdr.width = 1920;
    hdr.height = 1080;
    hdr.frame_rate = 30.0;
    hdr.frame_rate_fraction = "30/1";
    hdr.video_codec = "h264";
    hdr.color_space = ColorSpace::Pq;
    check(editor.apply(AddMedia{ std::move(hdr) }).has_value(), "adds");
    const nlohmann::json pq_doc = to_document(editor.project(), settings());
    const std::optional<Project> pq_loaded = from_document(pq_doc);
    check(pq_loaded.has_value() && pq_loaded->media.size() == 2
                  && pq_loaded->media[1].color_space == ColorSpace::Pq,
          "a file's colour round-trips");
    check(pq_loaded.has_value() && pq_loaded->media.size() == 2
                  && pq_loaded->media[0].color_space == ColorSpace::Sdr,
          "a plain import stays SDR");
}

// --- clamps shared with the command layer -----------------------------------

// A hand-edited document is clamped exactly where a command would have
// clamped: the reader trusts nothing past the same floors and ceilings.
void test_a_loaded_document_holds_the_same_ranges_every_command_does()
{
    auto [editor, media_id, clip_id] = fixture();
    (void)media_id;
    (void)clip_id;
    nlohmann::json document = to_document(editor.project(), settings());
    nlohmann::json &clip = document["timelines"][0]["clips"][0];
    clip["duration"] = 0.001;
    clip["scale"] = 50.0;
    clip["offsetX"] = -9.0;
    clip["rotation"] = 540.0;
    clip["speed"] = 100.0;
    const std::optional<Project> loaded = from_document(document);
    check(loaded.has_value(), "loads");
    if (!loaded.has_value()) {
        return;
    }
    const Clip &clamped = loaded->active().clips[0];
    check(clamped.duration == Rational{ 1, 60 }, "a sub-frame duration clamps to the floor");
    check(clamped.scale == 8.0, "a scale past the ceiling clamps to it");
    check(clamped.offset_x == -3.0, "an offset past the edge clamps to it");
    check(clamped.rotation == 180.0, "a rotation past a full turn wraps");
    check(clamped.speed == 16.0, "a speed past the ceiling clamps to it");
}

// --- key ease ---------------------------------------------------------------

// Keys shipped with four named eases before the curve editor replaced them
// with control points; both spellings have to load.
void test_a_key_reads_its_ease_named_or_numbered()
{
    const nlohmann::json document = nlohmann::json::parse(R"({
        "name": "Keyed", "version": 1,
        "media": [{ "id": "m1", "path": "/a.mp4", "name": "a.mp4",
                    "kind": "video", "duration": 10.0 }],
        "tracks": [{ "id": "T1", "name": "Track 1", "visible": true, "muted": false }],
        "clips": [{ "id": "c1", "trackId": "T1", "mediaId": "m1", "name": "a.mp4",
                    "kind": "video", "start": 0.0, "duration": 10.0, "sourceStart": 0.0,
                    "volume": 1.0, "fadeIn": 0.0, "fadeOut": 0.0, "scale": 1.0,
                    "offsetX": 0.0, "offsetY": 0.0, "rotation": 0.0, "opacity": 1.0,
                    "speed": 1.0, "preservePitch": true, "filters": [],
                    "keys": [
                        { "property": "opacity", "at": 0.0, "value": 0.0, "ease": "inOut" },
                        { "property": "opacity", "at": 0.5, "value": 1.0,
                          "ease": [0.1, 0.2, 0.3, 0.4] },
                        { "property": "opacity", "at": 1.0, "value": 0.5 },
                        { "property": "opacity", "at": 9.0, "value": 1.0 },
                        { "property": "nonesuch", "at": 0.2, "value": 1.0 }
                    ] }]
    })");
    const std::optional<Project> loaded = from_document(document);
    check(loaded.has_value(), "loads");
    if (!loaded.has_value()) {
        return;
    }
    const std::vector<ClipKey> &keys = loaded->active().clips[0].keys;
    check(keys.size() == 3,
          "the out-of-range key and the one naming an "
          "unknown property are dropped");
    check(keys.size() == 3 && keys[0].ease == KeyEase::in_out(), "a named ease reads");
    check(keys.size() == 3
                  && keys[1].ease == KeyEase{ std::array<double, 4>{ 0.1, 0.2, 0.3, 0.4 } },
          "a numbered ease reads");
    check(keys.size() == 3 && keys[2].ease == KeyEase::linear(),
          "an absent ease is a straight line, not a refusal to load");
}

// --- id minting across a reload ---------------------------------------------

// After loading a document, minting must never re-issue an id the file
// already uses. The host's editor cannot be built from a document, so the
// collision the Rust test guards is exercised on the IdMinter that adoption
// exists for: adopting every loaded id, then minting, stays clear of them.
void test_restored_ids_can_never_be_reissued()
{
    auto [editor, media_id, clip_id] = fixture();
    (void)media_id;
    (void)clip_id;
    const nlohmann::json document = to_document(editor.project(), settings());
    const std::optional<Project> restored = from_document(document);
    check(restored.has_value(), "loads");
    if (!restored.has_value()) {
        return;
    }
    IdMinter mint;
    mint.adopt_project(*restored);
    const std::string fresh = mint.next("m");
    const bool collides = std::any_of(restored->media.begin(), restored->media.end(),
                                      [&fresh](const MediaItem &item) { return item.id == fresh; });
    check(!collides, "the fresh id collides with nothing");
}

// --- command defaults -------------------------------------------------------

// A remove without the ripple field is a plain delete. The host models
// commands as structs rather than wire JSON, and the default member
// initializer is the same promise serde's default made: old callers do not
// send the field.
void test_a_remove_clips_document_without_the_ripple_field_still_parses_as_a_plain_delete()
{
    const RemoveClips remove{ std::vector<std::string>{ "c1" } };
    check(!remove.ripple, "the field is new; old callers do not send it");
}

void test_a_trim_clip_document_without_the_ripple_field_still_parses_as_a_plain_trim()
{
    const TrimClip trim{ "c1", TrimEdge::End, Rational{ -1, 1 } };
    check(!trim.ripple, "the field is new; old callers do not send it");
}

// --- template slots ---------------------------------------------------------

// A template slot's flag survives the document, and a document from a build
// that predates templates has no such field and reads as ordinary media.
void test_the_placeholder_flag_round_trips_and_defaults_off()
{
    auto [editor, media_id, clip_id] = fixture();
    (void)clip_id;
    check(editor.apply(SetMediaPlaceholder{ media_id, true }).has_value(), "marks");
    const nlohmann::json document = to_document(editor.project(), settings());
    const std::optional<Project> restored = from_document(document);
    check(restored.has_value() && !restored->media.empty() && restored->media[0].placeholder,
          "the flag survives the document");

    const nlohmann::json legacy = nlohmann::json::parse(R"({
        "name": "Old", "version": 1,
        "media": [{ "id": "m1", "path": "/a.mp4", "name": "a.mp4", "kind": "video",
                    "hasAudio": false }],
        "tracks": [{ "id": "T1", "name": "Track 1", "visible": true, "muted": false }],
        "clips": []
    })");
    const std::optional<Project> old = from_document(legacy);
    check(old.has_value() && !old->media.empty() && !old->media[0].placeholder,
          "a document before templates defaults the flag off");
}

// --- media origin -----------------------------------------------------------

// A generated file's origin round-trips; a plain import says nothing; and an
// origin this build does not know reads as an import, so the file is still a
// file.
void test_a_medias_origin_round_trips_and_an_unknown_one_reads_as_an_import()
{
    Editor editor;
    NewMedia voice;
    voice.path = "/voice.wav";
    voice.name = "voice.wav";
    voice.duration = Rational{ 3, 1 };
    voice.kind = MediaKind::Audio;
    voice.width = 1920;
    voice.height = 1080;
    voice.frame_rate = 30.0;
    voice.frame_rate_fraction = "30/1";
    voice.video_codec = "h264";
    voice.audio_codec = "aac";
    voice.has_audio = true;
    voice.origin = MediaOrigin::Speech;
    check(editor.apply(AddMedia{ std::move(voice) }).has_value(), "adds");
    check(editor.apply(media("/a.mp4", Rational{ 10, 1 }, true)).has_value(), "adds");

    const nlohmann::json document = to_document(editor.project(), settings());
    check(document["media"][0]["origin"] == "speech", "an origin writes as its word");
    check(document["media"][1].find("origin") == document["media"][1].end(),
          "an import says nothing, so a document without voices stays "
          "byte-identical");

    // An enhanced copy writes its own word too, and it reads back as Enhanced.
    NewMedia enhanced;
    enhanced.path = "/a-enhanced.webm";
    enhanced.name = "a-enhanced.webm";
    enhanced.duration = Rational{ 10, 1 };
    enhanced.kind = MediaKind::Video;
    enhanced.width = 2560;
    enhanced.height = 1440;
    enhanced.frame_rate = 30.0;
    enhanced.origin = MediaOrigin::Enhanced;
    check(editor.apply(AddMedia{ std::move(enhanced) }).has_value(), "adds an enhanced copy");
    const nlohmann::json with_enhanced = to_document(editor.project(), settings());
    check(with_enhanced["media"][2]["origin"] == "enhanced", "an enhanced copy writes as its word");

    const std::optional<Project> restored = from_document(document);
    check(restored.has_value() && restored->media.size() == 2
                  && restored->media[0].origin.has_value()
                  && *restored->media[0].origin == MediaOrigin::Speech,
          "the origin round-trips");
    check(restored.has_value() && restored->media.size() == 2
                  && !restored->media[1].origin.has_value(),
          "the import stays an import");

    const std::optional<Project> restored_enhanced = from_document(with_enhanced);
    check(restored_enhanced.has_value() && restored_enhanced->media.size() == 3
                  && restored_enhanced->media[2].origin.has_value()
                  && *restored_enhanced->media[2].origin == MediaOrigin::Enhanced,
          "the enhanced origin round-trips");

    const nlohmann::json legacy = nlohmann::json::parse(R"({
        "name": "Old", "version": 1,
        "media": [
            { "id": "m1", "path": "/a.mp4", "name": "a.mp4", "kind": "video",
              "hasAudio": false },
            { "id": "m2", "path": "/b.wav", "name": "b.wav", "kind": "audio",
              "hasAudio": true, "origin": "telepathy" },
            { "id": "m3", "path": "/c.wav", "name": "c.wav", "kind": "audio",
              "hasAudio": true, "origin": 7 }
        ],
        "tracks": [{ "id": "T1", "name": "Track 1", "visible": true, "muted": false }],
        "clips": []
    })");
    const std::optional<Project> old = from_document(legacy);
    const bool all_imports = old.has_value()
            && std::all_of(old->media.begin(), old->media.end(),
                           [](const MediaItem &item) { return !item.origin.has_value(); });
    check(all_imports, "an origin this build does not know reads as an import");
}

// --- effect keys ------------------------------------------------------------

// An effect key rides the knob at its own value and survives the document;
// once the keys come off, the knob falls back to its constant.
void test_effect_keys_ride_a_knob_and_survive_the_document()
{
    auto [editor, media_id, clip_id] = fixture();
    (void)media_id;

    AppliedFilter link = AppliedFilter::create("concat.adjust");
    link.params["exposure"] = 1.0;
    ClipPatch patch;
    patch.video_effects = std::vector<AppliedFilter>{ std::move(link) };
    check(editor.apply(UpdateClip{ clip_id, std::move(patch) }).has_value(), "applies");

    const double ats[2] = { 0.0, 1.0 };
    const double values[2] = { -1.0, 1.0 };
    for (std::size_t i = 0; i < 2; ++i) {
        check(editor.apply(SetEffectKey{ clip_id, 0, "exposure", ats[i], values[i],
                                         KeyEase::linear() })
                      .has_value(),
              "keys");
    }

    const AppliedFilter &effect = editor.project().active().clips[0].video_effects[0];
    check(effect.is_keyed("exposure"), "the knob rides");
    check(!effect.is_keyed("contrast"), "an unkeyed knob holds still");
    check(std::abs(effect.value_at("exposure", 0.5, 0.0)) < 1e-9,
          "halfway along a straight ride is zero");
    const auto at_quarter = effect.params_at(0.25);
    const auto quarter = at_quarter.find("exposure");
    check(quarter != at_quarter.end() && std::abs(quarter->second - (-0.5)) < 1e-9,
          "a quarter along is -0.5");
    const auto [before, after] = effect.keys_around("exposure", 0.5);
    check(before.has_value() && after.has_value() && std::abs(*before - 0.0) < 1e-9
                  && std::abs(*after - 1.0) < 1e-9,
          "the keys around halfway are the ends");
    check(effect.key_at("exposure", 1.0).has_value(), "the key at the end is found");

    const nlohmann::json document = to_document(editor.project(), settings());
    const std::optional<Project> restored = from_document(document);
    check(restored.has_value() && !restored->active().clips.empty()
                  && restored->active().clips[0].video_effects.size() == 1
                  && restored->active().clips[0].video_effects[0].keys_on("exposure").size() == 2,
          "the keys survive the document");

    // Clearing the keys returns the knob to the constant it holds.
    check(editor.apply(ClearEffectKey{ clip_id, 0, "exposure", 1.0 }).has_value(), "clears one");
    check(editor.apply(ClearEffectKeys{ clip_id, 0, "exposure" }).has_value(), "clears the rest");
    const AppliedFilter &cleared = editor.project().active().clips[0].video_effects[0];
    check(!cleared.is_keyed("exposure"), "the knob holds still again");
    check(std::abs(cleared.value_at("exposure", 0.5, 0.0) - 1.0) < 1e-9,
          "the constant is the fallback once the keys come off");
}

} // namespace

int main()
{
    test_a_document_from_a_newer_build_is_refused();
    test_a_timeline_without_its_own_frame_takes_the_documents();
    test_an_entry_the_reader_cannot_parse_is_dropped_not_the_document();
    test_the_document_round_trips();
    test_the_reverse_flag_round_trips_and_defaults_off();
    test_the_pan_round_trips_and_defaults_centred();
    test_a_version_one_document_loads();
    test_a_legacy_flat_document_loads_as_one_timeline();
    test_a_garbage_document_is_none_not_a_panic();
    test_clips_for_vanished_tracks_or_media_are_dropped_on_load();
    test_hand_edited_values_are_clamped_on_load();
    test_a_null_crop_on_the_wire_takes_the_crop_off();
    test_a_colour_space_round_trips();
    test_a_loaded_document_holds_the_same_ranges_every_command_does();
    test_a_key_reads_its_ease_named_or_numbered();
    test_restored_ids_can_never_be_reissued();
    test_a_remove_clips_document_without_the_ripple_field_still_parses_as_a_plain_delete();
    test_a_trim_clip_document_without_the_ripple_field_still_parses_as_a_plain_trim();
    test_the_placeholder_flag_round_trips_and_defaults_off();
    test_a_medias_origin_round_trips_and_an_unknown_one_reads_as_an_import();
    test_effect_keys_ride_a_knob_and_survive_the_document();

    std::printf("%d checks, %d failures\n", checks, failures);
    return failures == 0 ? 0 : 1;
}
