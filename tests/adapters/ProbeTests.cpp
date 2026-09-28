// SPDX-License-Identifier: GPL-3.0-or-later
// SPDX-FileCopyrightText: 2026 Genesis-0 contributors

#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <filesystem>
#include <optional>
#include <string>

#include <gst/gst.h>

#include "adapters/engine/ges/media/Probe.h"
#include "project/command/MediaCommands.h"
#include "project/model/Media.h"

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

using genesis::project::MediaKind;
using genesis::project::NewMedia;
namespace ges_adapter = genesis::adapters::engine::ges;

// The media file the probe must read: videotestsrc + audiotestsrc muxed to
// WebM, so the fixture carries both a picture and a sound stream. 30 frames at
// 30 fps is exactly one second, and the picture is 640x360 so the size
// assertions are not the encoder's default.
struct MediaFixture
{
    std::string path;
};

MediaFixture media_fixture()
{
    const std::filesystem::path generated =
            std::filesystem::temp_directory_path() / "genesis-probe-fixture.webm";
    std::filesystem::remove(generated);
    const std::string command = "gst-launch-1.0 -q "
                                "videotestsrc num-buffers=30 ! video/x-raw,width=640,height=360,"
                                "framerate=30/1 ! videoconvert ! vp8enc ! mux. "
                                "audiotestsrc num-buffers=43 ! audioconvert ! vorbisenc ! mux. "
                                "webmmux name=mux ! filesink location=\""
            + generated.string() + "\" >/dev/null 2>&1";
    const bool ok = std::system(command.c_str()) == 0 && std::filesystem::exists(generated)
            && std::filesystem::file_size(generated) > 0;
    return MediaFixture{ ok ? generated.string() : std::string{ } };
}

void test_probes_video_with_audio()
{
    const MediaFixture fixture = media_fixture();
    check(!fixture.path.empty(), "a media fixture is available");
    if (fixture.path.empty()) {
        return;
    }

    const std::optional<NewMedia> probed = ges_adapter::probe(fixture.path);
    check(probed.has_value(), "probes the fixture");
    if (!probed.has_value()) {
        std::filesystem::remove(fixture.path);
        return;
    }

    const NewMedia &media = *probed;
    check(media.kind == MediaKind::Video, "kind is Video");
    check(media.has_audio, "the fixture carries audio");
    check(media.width.has_value() && *media.width == 640, "width is 640");
    check(media.height.has_value() && *media.height == 360, "height is 360");
    check(media.name == "genesis-probe-fixture", "name is the file stem");

    check(media.duration.has_value(), "duration is reported");
    if (media.duration.has_value()) {
        const double seconds = media.duration->as_double();
        check(std::fabs(seconds - 1.0) <= 1.0 / 30.0, "duration is within a frame of one second");
    }

    check(media.frame_rate.has_value(), "frame rate is reported");
    if (media.frame_rate.has_value()) {
        check(std::fabs(*media.frame_rate - 30.0) < 1e-9, "frame rate is 30");
    }
    check(media.frame_rate_fraction.has_value(), "frame rate fraction is reported");
    if (media.frame_rate_fraction.has_value()) {
        check(*media.frame_rate_fraction == "30/1", "frame rate fraction is 30/1");
    }

    check(media.video_codec.has_value() && !media.video_codec->empty(), "video codec is non-empty");
    check(media.audio_codec.has_value() && !media.audio_codec->empty(), "audio codec is non-empty");
    if (media.video_codec.has_value()) {
        check(*media.video_codec == "vp8", "video codec is vp8");
    }
    if (media.audio_codec.has_value()) {
        check(*media.audio_codec == "vorbis", "audio codec is vorbis");
    }

    check(media.audio_tracks.size() == 1, "one audio track");
    if (!media.audio_tracks.empty()) {
        const auto &track = media.audio_tracks.front();
        check(track.channels == 1, "audio track is mono");
        check(track.sample_rate == 44100, "audio sample rate is 44100");
    }

    std::filesystem::remove(fixture.path);
}

void test_probes_a_missing_file_to_nullopt()
{
    const std::filesystem::path missing =
            std::filesystem::temp_directory_path() / "genesis-probe-does-not-exist.webm";
    std::filesystem::remove(missing);
    check(!ges_adapter::probe(missing).has_value(),
          "a nonexistent path probes to nullopt, not a crash");
}

} // namespace

int main()
{
    gst_init(nullptr, nullptr);

    test_probes_video_with_audio();
    test_probes_a_missing_file_to_nullopt();

    std::printf("%d checks, %d failures\n", checks, failures);
    return failures == 0 ? 0 : 1;
}
