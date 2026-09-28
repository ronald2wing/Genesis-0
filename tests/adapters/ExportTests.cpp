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
#include <gst/pbutils/pbutils.h>

#include "adapters/engine/Export.h"
#include "adapters/engine/ges/media/Probe.h"
#include "project/command/ClipCommands.h"
#include "project/command/Command.h"
#include "project/command/MediaCommands.h"
#include "project/command/PropertyCommands.h"
#include "project/model/Media.h"
#include "project/Editor.h"
#include "render/ExportSpec.h"
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
namespace ges_adapter = genesis::adapters::engine::ges;
namespace engine = genesis::adapters::engine;
using genesis::core::Rational;

// One frame at 30 fps, the tolerance for a duration reading.
constexpr double kFrameSeconds = 1.0 / 30.0;

// The first video stream's bit depth from the file's caps, or -1 when the file
// cannot be discovered or the depth is not signalled. HEVC's depth rides the
// parsed caps as bit-depth-luma; the profile ("main" vs "main-10") is the
// fallback for a mux that drops it.
int video_bit_depth(const std::filesystem::path &path)
{
    gchar *uri = gst_filename_to_uri(path.string().c_str(), nullptr);
    if (uri == nullptr) {
        return -1;
    }
    GstDiscoverer *discoverer = gst_discoverer_new(5 * GST_SECOND, nullptr);
    if (discoverer == nullptr) {
        g_free(uri);
        return -1;
    }
    GstDiscovererInfo *info = gst_discoverer_discover_uri(discoverer, uri, nullptr);
    g_free(uri);
    if (info == nullptr) {
        g_object_unref(discoverer);
        return -1;
    }
    int depth = -1;
    GList *streams = gst_discoverer_info_get_video_streams(info);
    if (streams != nullptr) {
        auto *video = static_cast<GstDiscovererVideoInfo *>(streams->data);
        GstCaps *caps = gst_discoverer_stream_info_get_caps(GST_DISCOVERER_STREAM_INFO(video));
        if (caps != nullptr && !gst_caps_is_empty(caps)) {
            const GstStructure *structure = gst_caps_get_structure(caps, 0);
            gint luma = 0;
            if (gst_structure_get_int(structure, "bit-depth-luma", &luma)) {
                depth = static_cast<int>(luma);
            } else if (const gchar *profile = gst_structure_get_string(structure, "profile")) {
                const std::string name(profile);
                depth = name.find("-12") != std::string::npos   ? 12
                        : name.find("-10") != std::string::npos ? 10
                                                                : 8;
            }
        }
    }
    gst_discoverer_info_unref(info);
    g_object_unref(discoverer);
    return depth;
}

// A one-second VP8/WebM source (the encoder guaranteed on this machine),
// generated on demand. An empty path fails the test loudly rather than skip.
struct MediaFixture
{
    std::string path;
    bool owns_file = false;
};

MediaFixture media_fixture()
{
    const std::filesystem::path generated =
            std::filesystem::temp_directory_path() / "genesis-export-fixture.webm";
    std::filesystem::remove(generated);
    const std::string command = "gst-launch-1.0 -q videotestsrc num-buffers=30 ! videoconvert ! "
                                "vp8enc ! webmmux ! filesink location=\""
            + generated.string() + "\" >/dev/null 2>&1";
    if (std::system(command.c_str()) == 0 && std::filesystem::exists(generated)
        && std::filesystem::file_size(generated) > 0) {
        return MediaFixture{ generated.string(), true };
    }
    std::filesystem::remove(generated);
    return MediaFixture{ "", false };
}

// Like media_fixture, but muxed with a Vorbis tone so the clip carries an
// audio stream - the precondition the audio export check needs.
MediaFixture audio_media_fixture()
{
    const std::filesystem::path generated =
            std::filesystem::temp_directory_path() / "genesis-export-audio-fixture.webm";
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

// The resolved host graph plus the flat clip list it came from, so the spec
// and the graph agree on the same clips.
struct ResolvedFixture
{
    render::BuiltTimeline built;
    std::vector<render::ExportClip> clips;
};

ResolvedFixture resolve(const std::string &media_path, std::uint32_t width, std::uint32_t height,
                        bool has_audio = false, std::vector<AppliedFilter> effects = { })
{
    NewMedia item;
    item.path = media_path;
    item.name = "fixture.webm";
    item.duration = Rational{ 1, 1 };
    item.kind = MediaKind::Video;
    item.width = 640;
    item.height = 360;
    item.has_audio = has_audio;
    Editor editor;
    const auto add_media = editor.apply(Command{ AddMedia{ std::move(item) } });
    check(add_media.has_value(), "adds media");
    check(add_media->created_id.has_value(), "mints an id");
    const std::string track_id = editor.project().timelines[0].tracks[0].id;
    const auto add_clip = editor.apply(
            Command{ AddClip{ *add_media->created_id, track_id, Rational{ 0, 1 }, false } });
    check(add_clip.has_value(), "adds clip");
    check(add_clip->created_id.has_value(), "mints an id");

    if (!effects.empty()) {
        ClipPatch patch;
        patch.video_effects = std::move(effects);
        check(editor.apply(Command{ UpdateClip{ *add_clip->created_id, std::move(patch) } })
                      .has_value(),
              "sets the clip effects");
    }

    const std::vector<render::ExportClip> clips =
            render::flatten_timeline(editor.project(), std::nullopt);
    check(clips.size() == 1, "one clip flattens");

    render::ExportRequest request;
    request.width = width;
    request.height = height;
    render::BuiltTimeline built =
            render::build_timeline(request, genesis::core::FrameRate::THIRTY,
                                   std::span<const render::ExportClip>(clips), { });
    check(built.timeline.clip_count() == 1, "one clip resolves");

    return ResolvedFixture{ std::move(built), std::move(clips) };
}

// The spec for one export of `clips` to `output` at `width`x`height`.
render::ExportSpec export_spec(const std::string &output, std::uint32_t width, std::uint32_t height,
                               const std::vector<render::ExportClip> &clips)
{
    render::ExportSpec spec;
    spec.output = output;
    spec.width = width;
    spec.height = height;
    spec.rate_num = 30;
    spec.rate_den = 1;
    spec.codec = render::ExportCodec::Vp8;
    spec.audio_codec = render::AudioCodec::Opus;
    spec.rate_mode = render::RateMode::Conform;
    spec.clips = clips;
    return spec;
}

void test_render_writes_the_requested_frame(const std::string &media_path)
{
    const std::filesystem::path output =
            std::filesystem::temp_directory_path() / "genesis-export-out.webm";
    std::filesystem::remove(output);

    const ResolvedFixture fixture = resolve(media_path, 320, 180);
    const render::ExportSpec spec = export_spec(output.string(), 320, 180, fixture.clips);

    double max_fraction = 0.0;
    int reports = 0;
    const auto progress = [&](engine::ExportProgress p) {
        ++reports;
        if (p.fraction > max_fraction) {
            max_fraction = p.fraction;
        }
    };

    const engine::ExportOutcome outcome =
            engine::export_timeline(spec, fixture.built, progress, { });

    check(outcome.ok, "the render succeeds");
    check(outcome.error.empty(), "a successful render has no error");
    check(std::filesystem::exists(output), "the output file exists");
    check(std::filesystem::file_size(output) > 0, "the output file is not empty");
    check(max_fraction >= 0.999, "progress reaches completion");
    check(reports > 0, "progress was reported at least once");

    const std::optional<NewMedia> probed = ges_adapter::probe(output);
    check(probed.has_value(), "the output probes");
    if (probed.has_value()) {
        check(probed->width.has_value() && *probed->width == 320, "the output is 320 wide");
        check(probed->height.has_value() && *probed->height == 180, "the output is 180 tall");
        if (probed->duration.has_value()) {
            const double seconds = probed->duration->as_double();
            check(std::fabs(seconds - 1.0) <= kFrameSeconds, "the output is about one second long");
        }
    }

    std::filesystem::remove(output);
}

// A clip whose effect is CPU-only (genesis.video-balance -> the stock
// `videobalance` element) still exports: the fallback installs through the
// builder and runs in the encode, so the output lands whole rather than the
// clip rendering effect-less or the export failing.
void test_cpu_fallback_clip_exports(const std::string &media_path)
{
    const std::filesystem::path output =
            std::filesystem::temp_directory_path() / "genesis-export-cpu.webm";
    std::filesystem::remove(output);

    AppliedFilter balance = AppliedFilter::create("genesis.video-balance");
    balance.params["brightness"] = -0.5;
    const ResolvedFixture fixture = resolve(media_path, 320, 180, false, { std::move(balance) });
    check(fixture.built.chains.size() == 1, "the clip's CPU-fallback chain resolves");

    const render::ExportSpec spec = export_spec(output.string(), 320, 180, fixture.clips);

    const engine::ExportOutcome outcome = engine::export_timeline(spec, fixture.built, { }, { });

    check(outcome.ok, "the CPU-fallback render succeeds");
    check(outcome.error.empty(), "a CPU-fallback render has no error");
    check(std::filesystem::exists(output), "the output file exists");
    check(std::filesystem::file_size(output) > 0, "the output file is not empty");

    std::filesystem::remove(output);
}

void test_cancel_stops_and_cleans_up(const std::string &media_path)
{
    const std::filesystem::path output =
            std::filesystem::temp_directory_path() / "genesis-export-cancel.webm";
    std::filesystem::remove(output);

    const ResolvedFixture fixture = resolve(media_path, 320, 180);
    const render::ExportSpec spec = export_spec(output.string(), 320, 180, fixture.clips);

    const auto cancelled = []() { return true; };
    const engine::ExportOutcome outcome =
            engine::export_timeline(spec, fixture.built, { }, cancelled);

    check(!outcome.ok, "a cancelled render does not succeed");
    check(!outcome.error.empty(), "a cancelled render reports a reason");
    check(!std::filesystem::exists(output), "no partial file is left behind");
}

void test_audio_bearing_export_has_audio(const std::string &media_path)
{
    const std::filesystem::path output =
            std::filesystem::temp_directory_path() / "genesis-export-audio.webm";
    std::filesystem::remove(output);

    const ResolvedFixture fixture = resolve(media_path, 320, 180, true);
    const render::ExportSpec spec = export_spec(output.string(), 320, 180, fixture.clips);

    const engine::ExportOutcome outcome = engine::export_timeline(spec, fixture.built, { }, { });

    check(outcome.ok, "the audio-bearing render succeeds");
    check(outcome.error.empty(), "an audio-bearing render has no error");
    check(std::filesystem::exists(output), "the audio output file exists");

    const std::optional<NewMedia> probed = ges_adapter::probe(output);
    check(probed.has_value(), "the audio output probes");
    if (probed.has_value()) {
        check(probed->has_audio, "the audio output carries an audio stream");
    }

    std::filesystem::remove(output);
}

void test_hevc_export_probes_as_hevc(const std::string &media_path)
{
    const std::filesystem::path output =
            std::filesystem::temp_directory_path() / "genesis-export-hevc.mp4";
    std::filesystem::remove(output);

    const ResolvedFixture fixture = resolve(media_path, 320, 180);
    render::ExportSpec spec = export_spec(output.string(), 320, 180, fixture.clips);
    spec.codec = render::ExportCodec::Hevc;

    const engine::ExportOutcome outcome = engine::export_timeline(spec, fixture.built, { }, { });

    check(outcome.ok, "the hevc render succeeds");
    check(outcome.error.empty(), "a hevc render has no error");
    check(std::filesystem::exists(output), "the hevc output file exists");

    const std::optional<NewMedia> probed = ges_adapter::probe(output);
    check(probed.has_value(), "the hevc output probes");
    if (probed.has_value() && probed->video_codec.has_value()) {
        check(*probed->video_codec == "h265", "the hevc output carries an h265 stream");
    }

    std::filesystem::remove(output);
}

void test_hevc_ten_bit_export_probes_as_ten_bit(const std::string &media_path)
{
    const std::filesystem::path output =
            std::filesystem::temp_directory_path() / "genesis-export-hevc10.mp4";
    std::filesystem::remove(output);

    const ResolvedFixture fixture = resolve(media_path, 320, 180);
    render::ExportSpec spec = export_spec(output.string(), 320, 180, fixture.clips);
    spec.codec = render::ExportCodec::Hevc;
    spec.ten_bit = true;

    const engine::ExportOutcome outcome = engine::export_timeline(spec, fixture.built, { }, { });

    check(outcome.ok, "the ten-bit hevc render succeeds");
    check(outcome.error.empty(), "a ten-bit hevc render has no error");
    check(std::filesystem::exists(output), "the ten-bit hevc output exists");
    check(video_bit_depth(output) == 10, "the ten-bit hevc output probes as 10-bit");

    std::filesystem::remove(output);
}

void test_ten_bit_without_a_ten_bit_encoder_is_refused(const std::string &media_path)
{
    // VP8 has no ten-bit encoder element, so a ten-bit request must refuse
    // rather than silently write 8-bit. This path is deterministic: it does
    // not depend on whether HEVC is installed on the machine.
    const std::filesystem::path output =
            std::filesystem::temp_directory_path() / "genesis-export-tenbit-refusal.webm";
    std::filesystem::remove(output);

    const ResolvedFixture fixture = resolve(media_path, 320, 180);
    render::ExportSpec spec = export_spec(output.string(), 320, 180, fixture.clips);
    spec.ten_bit = true; // codec stays Vp8

    const engine::ExportOutcome outcome = engine::export_timeline(spec, fixture.built, { }, { });

    check(!outcome.ok, "a ten-bit request over a codec with no ten-bit encoder refuses");
    check(outcome.error.find("10-bit") != std::string::npos,
          "the refusal names the missing ten-bit support");
    check(!std::filesystem::exists(output), "no output is written on refusal");
}

} // namespace

int main()
{
    gst_init(nullptr, nullptr);
    ges_init();

    const MediaFixture media = media_fixture();
    check(!media.path.empty(), "a media fixture is available");
    if (!media.path.empty()) {
        test_render_writes_the_requested_frame(media.path);
        test_cpu_fallback_clip_exports(media.path);
        test_cancel_stops_and_cleans_up(media.path);
        test_hevc_export_probes_as_hevc(media.path);
        test_hevc_ten_bit_export_probes_as_ten_bit(media.path);
        test_ten_bit_without_a_ten_bit_encoder_is_refused(media.path);
    }
    if (media.owns_file) {
        std::filesystem::remove(media.path);
    }

    const MediaFixture audio_media = audio_media_fixture();
    check(!audio_media.path.empty(), "an audio media fixture is available");
    if (!audio_media.path.empty()) {
        test_audio_bearing_export_has_audio(audio_media.path);
    }
    if (audio_media.owns_file) {
        std::filesystem::remove(audio_media.path);
    }

    std::printf("%d checks, %d failures\n", checks, failures);
    return failures == 0 ? 0 : 1;
}
