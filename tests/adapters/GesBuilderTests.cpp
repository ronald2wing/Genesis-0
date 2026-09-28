// SPDX-License-Identifier: GPL-3.0-or-later
// SPDX-FileCopyrightText: 2026 Genesis-0 contributors

#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <filesystem>
#include <optional>
#include <span>
#include <string>
#include <utility>
#include <vector>

#include <ges/ges.h>
#include <gst/gst.h>

#include "adapters/engine/ges/session/GesBuilder.h"
#include "project/command/ClipCommands.h"
#include "project/command/Command.h"
#include "project/command/MediaCommands.h"
#include "project/model/Media.h"
#include "project/Editor.h"
#include "render/Flatten.h"
#include "render/Resolve.h"
#include "../support/Checks.h"

namespace {

using genesis::test::check;

using namespace genesis::project;
namespace render = genesis::render;
namespace ges_adapter = genesis::adapters::engine::ges;
using genesis::core::Rational;

// The rational -> GES nanosecond seam: `to_ns` in GesBuilder.cpp rounds the
// host's exact rational to the nearest nanosecond. A correct build is exact
// to within one nanosecond, so the duration assertion tolerates that seam and
// nothing else.
constexpr GstClockTime kDurationToleranceNanos = 1;

// The media file the asset request must actually load. Generated on demand as
// a one-second VP8/WebM (the only encoder guaranteed on this machine), with
// the Phase 0 spike file as a documented fallback. Returns an empty path when
// neither is possible, and the test then fails loudly rather than skip.
struct MediaFixture
{
    std::string path;
    bool owns_file = false;
};

MediaFixture media_fixture()
{
    const std::filesystem::path generated =
            std::filesystem::temp_directory_path() / "genesis-ges-builder-fixture.webm";
    std::filesystem::remove(generated);
    // 30 frames of testsrc at 30 fps is exactly one second.
    const std::string command = "gst-launch-1.0 -q videotestsrc num-buffers=30 ! videoconvert ! "
                                "vp8enc ! webmmux ! filesink location=\""
            + generated.string() + "\" >/dev/null 2>&1";
    if (std::system(command.c_str()) == 0 && std::filesystem::exists(generated)
        && std::filesystem::file_size(generated) > 0) {
        return MediaFixture{ generated.string(), true };
    }
    std::filesystem::remove(generated);
    const std::filesystem::path spike =
            std::filesystem::temp_directory_path() / "ges-spike" / "a.webm";
    if (std::filesystem::exists(spike)) {
        return MediaFixture{ spike.string(), false };
    }
    return MediaFixture{ "", false };
}

// Like media_fixture, but muxed with a Vorbis tone so the GES clip materialises
// an audio source track element - the precondition the audio wiring test needs.
MediaFixture audio_media_fixture()
{
    const std::filesystem::path generated =
            std::filesystem::temp_directory_path() / "genesis-ges-builder-audio-fixture.webm";
    std::filesystem::remove(generated);
    const std::string command = "gst-launch-1.0 -q videotestsrc num-buffers=30 ! videoconvert ! "
                                "vp8enc ! webmmux name=mux ! filesink location=\""
            + generated.string()
            + "\" audiotestsrc num-buffers=30 samplesperbuffer=1600 ! audioconvert "
              "! vorbisenc ! mux. >/dev/null 2>&1";
    if (std::system(command.c_str()) == 0 && std::filesystem::exists(generated)
        && std::filesystem::file_size(generated) > 0) {
        return MediaFixture{ generated.string(), true };
    }
    std::filesystem::remove(generated);
    return MediaFixture{ "", false };
}

// A project with one video media item and one clip of it, built through the
// real command layer so the fixture cannot drift from the model.
struct ProjectFixture
{
    Editor editor;
    std::string media_id;
    std::string clip_id;
};

ProjectFixture project_fixture(const std::string &media_path, bool has_audio)
{
    ProjectFixture f;
    NewMedia item;
    item.path = media_path;
    item.name = "fixture.webm";
    item.duration = Rational{ 1, 1 };
    item.kind = MediaKind::Video;
    item.width = 640;
    item.height = 360;
    item.has_audio = has_audio;
    const auto add_media = f.editor.apply(Command{ AddMedia{ std::move(item) } });
    check(add_media.has_value(), "adds media");
    check(add_media->created_id.has_value(), "mints an id");
    f.media_id = *add_media->created_id;
    const std::string track_id = f.editor.project().timelines[0].tracks[0].id;
    const auto add_clip =
            f.editor.apply(Command{ AddClip{ f.media_id, track_id, Rational{ 0, 1 }, false } });
    check(add_clip.has_value(), "adds clip");
    check(add_clip->created_id.has_value(), "mints an id");
    f.clip_id = *add_clip->created_id;
    return f;
}

void test_builds_the_host_graph_into_a_gestimeline()
{
    const MediaFixture media = media_fixture();
    check(!media.path.empty(), "a media fixture is available");
    if (media.path.empty()) {
        return;
    }

    const ProjectFixture fixture = project_fixture(media.path, false);
    (void)fixture.clip_id;

    const std::vector<render::ExportClip> flat =
            render::flatten_timeline(fixture.editor.project(), std::nullopt);
    check(flat.size() == 1, "one clip flattens");

    render::ExportRequest request;
    request.width = 640;
    request.height = 360;
    const render::BuiltTimeline built =
            render::build_timeline(request, genesis::core::FrameRate::THIRTY,
                                   std::span<const render::ExportClip>(flat), { });

    check(built.timeline.clip_count() == 1, "one clip resolves");

    GESTimeline *timeline = ges_adapter::build_timeline(built);
    check(timeline != nullptr, "the GES timeline builds");
    if (timeline == nullptr) {
        if (media.owns_file) {
            std::filesystem::remove(media.path);
        }
        return;
    }

    GList *layers = ges_timeline_get_layers(timeline);
    check(g_list_length(layers) == 1, "one GES layer per host track");
    guint clip_total = 0;
    for (GList *it = layers; it != nullptr; it = it->next) {
        GList *clips = ges_layer_get_clips(GES_LAYER(it->data));
        clip_total += g_list_length(clips);
        g_list_free(clips);
    }
    check(clip_total == 1, "one GES clip for the one host clip");
    g_list_free(layers);

    const GstClockTime expected_ns = static_cast<GstClockTime>(
            std::llround(built.timeline.duration().as_double() * GST_SECOND));
    const GstClockTime actual_ns = ges_timeline_get_duration(timeline);
    check(actual_ns >= expected_ns - kDurationToleranceNanos
                  && actual_ns <= expected_ns + kDurationToleranceNanos,
          "the GES duration matches the host span at the seam tolerance");

    gst_object_unref(timeline);

    if (media.owns_file) {
        std::filesystem::remove(media.path);
    }
}

void test_audio_bearing_clip_wires_gain_pan_and_fades()
{
    const MediaFixture media = audio_media_fixture();
    check(!media.path.empty(), "an audio media fixture is available");
    if (media.path.empty()) {
        return;
    }

    ProjectFixture fixture = project_fixture(media.path, true);

    // Gain and head/tail ramps through the patch command, pan through its own.
    ClipPatch patch;
    patch.volume = 0.5;
    patch.fade_in = Rational{ 1, 4 };
    patch.fade_out = Rational{ 1, 4 };
    check(fixture.editor.apply(Command{ UpdateClip{ fixture.clip_id, patch } }).has_value(),
          "sets gain and fades");
    check(fixture.editor.apply(Command{ SetClipPan{ fixture.clip_id, 0.5 } }).has_value(),
          "sets the pan");

    const std::vector<render::ExportClip> flat =
            render::flatten_timeline(fixture.editor.project(), std::nullopt);
    check(flat.size() == 1, "one clip flattens");

    render::ExportRequest request;
    request.width = 640;
    request.height = 360;
    const render::BuiltTimeline built =
            render::build_timeline(request, genesis::core::FrameRate::THIRTY,
                                   std::span<const render::ExportClip>(flat), { });

    check(built.audio.size() == 1, "one clip carries audio facts");

    GESTimeline *timeline = ges_adapter::build_timeline(built);
    check(timeline != nullptr, "the GES timeline builds");
    if (timeline == nullptr) {
        if (media.owns_file) {
            std::filesystem::remove(media.path);
        }
        return;
    }

    // The sole clip must own an audio source track element.
    GList *layers = ges_timeline_get_layers(timeline);
    GESLayer *layer = GES_LAYER(layers->data);
    GList *clips = ges_layer_get_clips(layer);
    check(g_list_length(clips) == 1, "one GES clip");
    GESClip *clip = GES_CLIP(clips->data);

    GESTrackElement *audio_source = nullptr;
    GList *children = ges_container_get_children(GES_CONTAINER(clip), TRUE);
    for (GList *it = children; it != nullptr; it = it->next) {
        if (GES_IS_AUDIO_SOURCE(it->data)) {
            audio_source = GES_TRACK_ELEMENT(it->data);
            break;
        }
    }
    g_list_free(children);
    check(audio_source != nullptr, "the clip has an audio source element");

    if (audio_source != nullptr) {
        GValue volume = G_VALUE_INIT;
        g_value_init(&volume, G_TYPE_DOUBLE);
        check(ges_timeline_element_get_child_property(GES_TIMELINE_ELEMENT(audio_source), "volume",
                                                      &volume)
                      && g_value_get_double(&volume) == 0.5,
              "the gain lands on the audio source");
        g_value_unset(&volume);

        // The control binding `ges_track_element_set_control_source` creates
        // for the child property "volume" lives in GES's own per-track-element
        // binding table, not on the GstObject the track element is cast to (the
        // nleobject has none either - it is a child property binding). The
        // lookup that actually finds it is GES's own accessor.
        check(ges_track_element_get_control_binding(audio_source, "volume") != nullptr,
              "the fades ride a volume control source");
    }

    // The pan rides an audiopanorama top effect whose `panorama` reads back in
    // range. No other top effect carries that child property, so it identifies
    // the pan unambiguously.
    bool pan_ok = false;
    GList *effects = ges_clip_get_top_effects(clip);
    for (GList *it = effects; it != nullptr && !pan_ok; it = it->next) {
        if (!GES_IS_EFFECT(it->data)) {
            continue;
        }
        GValue panorama = G_VALUE_INIT;
        g_value_init(&panorama, G_TYPE_FLOAT);
        if (ges_timeline_element_get_child_property(GES_TIMELINE_ELEMENT(it->data), "panorama",
                                                    &panorama)) {
            const gfloat value = g_value_get_float(&panorama);
            pan_ok = value > 0.4f && value < 0.6f;
        }
        g_value_unset(&panorama);
    }
    g_list_free(effects);
    check(pan_ok, "the pan rides an audiopanorama top effect");

    g_list_free(clips);
    g_list_free(layers);
    gst_object_unref(timeline);

    if (media.owns_file) {
        std::filesystem::remove(media.path);
    }
}

void test_an_audio_filter_pack_wires_an_rgvolume_effect()
{
    const MediaFixture media = audio_media_fixture();
    check(!media.path.empty(), "an audio media fixture is available");
    if (media.path.empty()) {
        return;
    }

    ProjectFixture fixture = project_fixture(media.path, true);

    // The loudness pack applied with a +6 dB boost on its `boost` knob.
    AppliedFilter loudness = AppliedFilter::create("genesis.loudness");
    loudness.params["boost"] = 6.0;
    ClipPatch patch;
    patch.filters = std::vector<AppliedFilter>{ loudness };
    check(fixture.editor.apply(Command{ UpdateClip{ fixture.clip_id, patch } }).has_value(),
          "sets an audio filter");

    const std::vector<render::ExportClip> flat =
            render::flatten_timeline(fixture.editor.project(), std::nullopt);
    check(flat.size() == 1, "one clip flattens");
    check(flat[0].audio_filters.size() == 1, "one audio filter flattens");

    render::ExportRequest request;
    request.width = 640;
    request.height = 360;
    const render::BuiltTimeline built =
            render::build_timeline(request, genesis::core::FrameRate::THIRTY,
                                   std::span<const render::ExportClip>(flat), { });

    check(built.audio.size() == 1, "one clip carries audio facts");
    check(built.audio_chains.size() == 1, "one clip carries audio effects");

    GESTimeline *timeline = ges_adapter::build_timeline(built);
    check(timeline != nullptr, "the GES timeline builds");
    if (timeline == nullptr) {
        if (media.owns_file) {
            std::filesystem::remove(media.path);
        }
        return;
    }

    GList *layers = ges_timeline_get_layers(timeline);
    GESLayer *layer = GES_LAYER(layers->data);
    GList *clips = ges_layer_get_clips(layer);
    check(g_list_length(clips) == 1, "one GES clip");
    GESClip *clip = GES_CLIP(clips->data);

    // The rgvolume top effect must carry the +6 dB pre-amp the pack mapped
    // from `boost`. No other top effect has a `pre-amp` child property, so it
    // identifies the loudness effect unambiguously.
    bool loudness_ok = false;
    GList *effects = ges_clip_get_top_effects(clip);
    for (GList *it = effects; it != nullptr && !loudness_ok; it = it->next) {
        if (!GES_IS_EFFECT(it->data)) {
            continue;
        }
        GValue pre_amp = G_VALUE_INIT;
        g_value_init(&pre_amp, G_TYPE_DOUBLE);
        if (ges_timeline_element_get_child_property(GES_TIMELINE_ELEMENT(it->data), "pre-amp",
                                                    &pre_amp)) {
            loudness_ok = g_value_get_double(&pre_amp) == 6.0;
        }
        g_value_unset(&pre_amp);
    }
    g_list_free(effects);
    check(loudness_ok, "the boost lands on an rgvolume top effect");

    g_list_free(clips);
    g_list_free(layers);
    gst_object_unref(timeline);

    if (media.owns_file) {
        std::filesystem::remove(media.path);
    }
}

} // namespace

int main()
{
    gst_init(nullptr, nullptr);
    ges_init();

    test_builds_the_host_graph_into_a_gestimeline();
    test_audio_bearing_clip_wires_gain_pan_and_fades();
    test_an_audio_filter_pack_wires_an_rgvolume_effect();

    return genesis::test::summary();
}
