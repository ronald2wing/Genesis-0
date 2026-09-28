// SPDX-License-Identifier: GPL-3.0-or-later
// SPDX-FileCopyrightText: 2026 Genesis-0 contributors

#include <array>
#include <cstdio>
#include <optional>
#include <string>
#include <utility>
#include <vector>

#include <nlohmann/json.hpp>

#include "../support/Checks.h"
#include "project/command/Codec.h"
#include "project/command/Command.h"
#include "project/model/Effects.h"
#include "project/model/Keyframe.h"
#include "project/model/Speed.h"

namespace {

using genesis::test::check;
namespace command = genesis::project::command;
namespace project = genesis::project;

using genesis::core::Rational;

// Encodes then decodes `cmd` and asserts the result is exactly `cmd`.
void round_trips(const project::Command &cmd, const std::string &what)
{
    const nlohmann::json encoded = command::encode(cmd);
    const std::optional<project::Command> decoded = command::decode(encoded);
    check(decoded.has_value(), what + ": decodes");
    if (decoded) {
        check(*decoded == cmd, what + ": round-trips exactly");
    }
}

void test_every_command_round_trips()
{
    round_trips(project::Command{ project::AddMedia{ } }, "addMedia (empty item)");
    round_trips(project::Command{ project::RemoveMedia{ "m1" } }, "removeMedia");
    round_trips(project::Command{ project::SetMediaPlaceholder{ "m1", true } },
                "setMediaPlaceholder");
    round_trips(project::Command{ project::FillSlot{ "m1", project::NewMedia{ } } }, "fillSlot");
    round_trips(project::Command{ project::Batch{ std::vector<project::Command>{
                        project::Command{ project::AddTrack{ } },
                        project::Command{ project::RemoveMedia{ "m2" } },
                } } },
                "batch (nested)");
    round_trips(project::Command{ project::AddClip{ "m1", "t1", Rational::from_int(4), true } },
                "addClip");
    round_trips(project::Command{ project::AddClipAtFirstFree{ "m1", Rational::from_int(2) } },
                "addClipAtFirstFree");
    round_trips(project::Command{ project::AddTextClip{ } }, "addTextClip (defaults)");
    round_trips(project::Command{ project::AddLayerClip{ } }, "addLayerClip (defaults)");
    round_trips(project::Command{ project::SetClipSpeedCurve{ } }, "setClipSpeedCurve (no curve)");
    round_trips(project::Command{ project::SetClipKey{ } }, "setClipKey (defaults)");
    round_trips(project::Command{ project::ClearClipKey{ } }, "clearClipKey (defaults)");
    round_trips(project::Command{ project::ClearClipKeys{ } }, "clearClipKeys (defaults)");
    round_trips(project::Command{ project::SetEffectKey{ } }, "setEffectKey (defaults)");
    round_trips(project::Command{ project::ClearEffectKey{ } }, "clearEffectKey (defaults)");
    round_trips(project::Command{ project::ClearEffectKeys{ } }, "clearEffectKeys (defaults)");
    round_trips(project::Command{ project::SetClipCutout{ } }, "setClipCutout (no cutout)");
    round_trips(project::Command{ project::AddCutoutStroke{ "c1", project::Stroke{ } } },
                "addCutoutStroke (defaults)");
    round_trips(project::Command{ project::MoveClips{ } }, "moveClips (empty)");
    round_trips(project::Command{ project::TrimClip{ } }, "trimClip (defaults)");
    round_trips(project::Command{ project::SplitClips{ } }, "splitClips (defaults)");
    round_trips(project::Command{ project::ReplaceClipMedia{ "c1", project::NewMedia{ },
                                                             std::nullopt } },
                "replaceClipMedia");
    round_trips(project::Command{ project::FreezeFrame{ } }, "freezeFrame (defaults)");
    round_trips(project::Command{ project::MergeClips{ } }, "mergeClips (empty)");
    round_trips(project::Command{ project::RemoveClips{ } }, "removeClips (empty)");
    round_trips(project::Command{ project::UpdateClip{ } }, "updateClip (defaults)");
    round_trips(project::Command{ project::SetClipSpeed{ } }, "setClipSpeed (defaults)");
    round_trips(project::Command{ project::SetClipReverse{ } }, "setClipReverse (defaults)");
    round_trips(project::Command{ project::SetClipPan{ } }, "setClipPan (defaults)");
    round_trips(project::Command{ project::SetClipTransform{ } }, "setClipTransform (defaults)");
    round_trips(project::Command{ project::DetachAudio{ "c1" } }, "detachAudio");
    round_trips(project::Command{ project::ReattachAudio{ "c1" } }, "reattachAudio");
    round_trips(project::Command{ project::AddTrack{ } }, "addTrack");
    round_trips(project::Command{ project::RemoveTrack{ "t1" } }, "removeTrack");
    round_trips(project::Command{ project::SetTrackFlag{ } }, "setTrackFlag (defaults)");
    round_trips(project::Command{ project::AddTimeline{ } }, "addTimeline");
    round_trips(project::Command{ project::RemoveTimeline{ "tl1" } }, "removeTimeline");
    round_trips(project::Command{ project::SetTimelineVideo{ } }, "setTimelineVideo (defaults)");
    round_trips(project::Command{ project::RenameTimeline{ "tl1", "New" } }, "renameTimeline");
    round_trips(project::Command{ project::SelectTimeline{ "tl1" } }, "selectTimeline");
    round_trips(project::Command{ project::MoveTimeline{ "tl1", 3 } }, "moveTimeline");
    round_trips(project::Command{ project::AddFont{ "Fam", "/f.ttf" } }, "addFont");
    round_trips(project::Command{ project::RemoveFont{ "Fam" } }, "removeFont");
    round_trips(project::Command{ project::UpdateMediaPath{ "m1", "/new.mp4" } },
                "updateMediaPath");
    round_trips(project::Command{ project::SetMediaColorRange{ "m1", std::nullopt } },
                "setMediaColorRange (null range)");
}

void test_fully_populated_commands_round_trip()
{
    project::AudioTrack track;
    track.index = 0;
    track.codec = "aac";
    track.channels = 2;
    track.sample_rate = 48000;
    track.title = "Mic";
    track.language = "eng";

    project::NewMedia media;
    media.path = "/media/a.mp4";
    media.name = "a.mp4";
    media.duration = Rational(30000, 1001);
    media.kind = project::MediaKind::Video;
    media.width = 1920;
    media.height = 1080;
    media.frame_rate = 29.97;
    media.frame_rate_fraction = "30000/1001";
    media.video_codec = "h264";
    media.audio_codec = "aac";
    media.has_audio = true;
    media.audio_tracks.push_back(track);
    media.origin = project::MediaOrigin::Enhanced;
    media.color_space = project::ColorSpace::Hlg;

    round_trips(project::Command{ project::AddMedia{ media } }, "addMedia (fully populated)");

    project::Stroke stroke;
    stroke.tool = project::BrushTool::SmartEraser;
    stroke.size = 0.1;
    stroke.points = { { 0.1, 0.2 }, { 0.3, 0.4 } };
    stroke.at = Rational::from_int(1);

    project::Cutout cutout;
    cutout.mode = project::CutoutMode::Custom;
    cutout.subject = project::Subject::Person;
    cutout.feather = 0.05;
    cutout.strokes.push_back(stroke);

    round_trips(project::Command{ project::SetClipCutout{ "c1", cutout } },
                "setClipCutout (custom)");
    round_trips(project::Command{ project::AddCutoutStroke{ "c1", stroke } },
                "addCutoutStroke (smart, timed)");

    project::ParamKey key;
    key.at = 0.25;
    key.value = 0.5;
    key.ease = project::KeyEase::out();

    project::AppliedFilter filter;
    filter.id = "sepia";
    filter.params["amount"] = 0.75;
    filter.enabled = false;
    filter.keys["amount"].push_back(key);

    project::ClipPatch patch;
    patch.name = "hello";
    patch.volume = 1.5;
    patch.fade_in = Rational::from_int(1);
    patch.opacity = 0.5;
    patch.preserve_pitch = true;
    patch.flip_h = true;
    patch.crop = project::Crop{ 0.1, 0.2, 0.3, 0.4 };
    patch.filters = { filter };
    patch.transition_in = project::Transition{ "cross-fade", Rational::from_int(2) };
    patch.audio_stream = std::uint32_t{ 1 };

    round_trips(project::Command{ project::UpdateClip{ "c1", patch } },
                "updateClip (fully populated)");

    project::TextStyle style;
    style.content = "Title";
    style.font_size = 0.12;
    style.align = project::TextAlign::Left;
    round_trips(project::Command{ project::AddTextClip{ "t1", true, Rational::from_int(3), style,
                                                        Rational::from_int(5), 0.25 } },
                "addTextClip (fully populated)");

    project::SpeedPoint point;
    point.at = 0.5;
    point.speed = 2.0;
    round_trips(project::Command{ project::SetClipSpeedCurve{
                        "c1", std::vector<project::SpeedPoint>{ point } } },
                "setClipSpeedCurve (one point)");

    project::VideoSettings video;
    video.width = 3840;
    video.height = 2160;
    video.rate_num = 60000;
    video.rate_den = 1001;
    video.color_space = project::ColorSpace::Pq;
    round_trips(project::Command{ project::SetTimelineVideo{ "tl1", video } },
                "setTimelineVideo (HDR)");

    project::ClipMove move;
    move.clip_id = "c1";
    move.start = Rational(30000, 1001);
    move.track_id = "t2";
    round_trips(project::Command{ project::MoveClips{ std::vector<project::ClipMove>{ move } } },
                "moveClips (one move)");

    round_trips(project::Command{ project::SetMediaColorRange{ "m1", project::ColorRange::Full } },
                "setMediaColorRange (full)");
}

void test_key_commands_round_trip()
{
    round_trips(project::Command{ project::SetClipKey{ "c1", project::KeyProperty::Opacity, 0.5,
                                                       0.8, project::KeyEase::in_out() } },
                "setClipKey (inOut ease)");
    round_trips(
            project::Command{ project::ClearClipKey{ "c1", project::KeyProperty::OffsetX, 0.25 } },
            "clearClipKey");
    round_trips(project::Command{ project::ClearClipKeys{ "c1", project::KeyProperty::Volume } },
                "clearClipKeys");
    round_trips(project::Command{ project::SetEffectKey{ "c1", std::size_t{ 2 }, "amount", 0.5, 0.9,
                                                         project::KeyEase::out() } },
                "setEffectKey");
    round_trips(
            project::Command{ project::ClearEffectKey{ "c1", std::size_t{ 1 }, "amount", 0.5 } },
            "clearEffectKey");
    round_trips(project::Command{ project::ClearEffectKeys{ "c1", std::size_t{ 0 }, "amount" } },
                "clearEffectKeys");
}

void test_rejects_non_object_or_missing_op()
{
    check(!command::decode(nlohmann::json::array()), "a non-object rejects");
    check(!command::decode(nlohmann::json{ 5 }), "a scalar rejects");
    check(!command::decode(nlohmann::json{ { "x", 1 } }), "a missing op rejects");
    check(!command::decode(nlohmann::json{ { "op", 5 } }), "a non-string op rejects");
    check(!command::decode(nlohmann::json{ { "op", "noSuchOp" } }), "an unknown op rejects");
}

void test_rejects_malformed_elements()
{
    // A wrong-typed scalar rejects the whole command.
    check(!command::decode(nlohmann::json{ { "op", "removeMedia" }, { "mediaId", 5 } }),
          "a wrong-typed scalar rejects");
    // An unknown enum spelling rejects.
    check(!command::decode(nlohmann::json{
                  { "op", "addMedia" }, { "item", { { "path", "/a" }, { "kind", "hologram" } } } }),
          "an unknown media kind rejects");
    // A missing required nested struct rejects.
    check(!command::decode(nlohmann::json{ { "op", "addMedia" } }), "a missing item rejects");
    // A bad ease string (not a preset name) rejects.
    check(!command::decode(nlohmann::json{ { "op", "setClipKey" },
                                           { "clipId", "c1" },
                                           { "property", "opacity" },
                                           { "at", 0.5 },
                                           { "value", 1.0 },
                                           { "ease", "linear" } }),
          "an ease named linear rejects");
    // A bad ease array (wrong length) rejects.
    check(!command::decode(nlohmann::json{ { "op", "setClipKey" },
                                           { "clipId", "c1" },
                                           { "property", "opacity" },
                                           { "at", 0.5 },
                                           { "value", 1.0 },
                                           { "ease", nlohmann::json::array({ 0.0, 1.0 }) } }),
          "a 2-number ease array rejects");
    // A non-finite ease value rejects.
    const std::string nan_json =
            R"({"op":"setClipKey","clipId":"c1","property":"opacity","at":0.5,"value":1.0,"ease":[0,null,1,1]})";
    check(!command::decode(nlohmann::json::parse(nan_json)), "a non-numeric ease entry rejects");
    // A bad list entry rejects the whole list.
    check(!command::decode(nlohmann::json{ { "op", "mergeClips" },
                                           { "clipIds", nlohmann::json::array({ "c1", 7 }) } }),
          "a bad list entry rejects");
}

void test_double_optionals_keep_three_way_semantics()
{
    const project::Command clear =
            project::Command{ project::UpdateClip{ "c1", project::ClipPatch{ } } };
    const nlohmann::json clear_json = command::encode(clear);
    // A default patch: the plain optionals are null, the double-optionals absent.
    check(clear_json["patch"]["name"].is_null(), "an unset name is null");
    check(!clear_json["patch"].contains("crop"), "an unset crop is omitted");
    check(!clear_json["patch"].contains("transitionIn"), "an unset transitionIn is omitted");
    check(!clear_json["patch"].contains("text"), "an unset text is omitted");
    check(!clear_json["patch"].contains("audioStream"), "an unset audioStream is omitted");

    // null clears a double-optional.
    project::ClipPatch cleared;
    cleared.crop = std::optional<project::Crop>{ };
    const nlohmann::json cleared_json =
            command::encode(project::Command{ project::UpdateClip{ "c1", cleared } });
    check(cleared_json["patch"]["crop"].is_null(), "an explicit null crop clears");
    const std::optional<project::Command> cleared_decoded = command::decode(cleared_json);
    check(cleared_decoded.has_value(), "a cleared crop decodes");
    if (cleared_decoded) {
        const auto *update = std::get_if<project::UpdateClip>(&cleared_decoded->value);
        check(update && update->patch.crop.has_value() && !update->patch.crop->has_value(),
              "a cleared crop stays cleared (not untouched)");
    }
}

void test_encode_matches_the_wire_shape()
{
    const nlohmann::json add_track = command::encode(project::Command{ project::AddTrack{ } });
    check(add_track == nlohmann::json{ { "op", "addTrack" } }, "addTrack encodes as just its op");

    const nlohmann::json remove = command::encode(project::Command{ project::RemoveMedia{ "m1" } });
    check(remove == nlohmann::json{ { "op", "removeMedia" }, { "mediaId", "m1" } },
          "removeMedia encodes op + mediaId");

    // A time is always the exact "num/den" string.
    const nlohmann::json add_clip = command::encode(
            project::Command{ project::AddClip{ "m1", "t1", Rational(30000, 1001), false } });
    check(add_clip["start"] == "30000/1001", "a rational time encodes as the exact num/den string");
}

} // namespace

int main()
{
    test_every_command_round_trips();
    test_fully_populated_commands_round_trip();
    test_key_commands_round_trip();
    test_rejects_non_object_or_missing_op();
    test_rejects_malformed_elements();
    test_double_optionals_keep_three_way_semantics();
    test_encode_matches_the_wire_shape();

    return genesis::test::summary();
}
