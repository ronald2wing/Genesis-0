// SPDX-License-Identifier: GPL-3.0-or-later
// SPDX-FileCopyrightText: 2026 Genesis-0 contributors

#include <chrono>
#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <filesystem>
#include <optional>
#include <span>
#include <string>
#include <thread>
#include <utility>
#include <vector>

#include <ges/ges.h>
#include <gst/gst.h>
#include <gst/gl/egl/gstgldisplay_egl.h>
#include <gst/gl/gl.h>

#include "adapters/engine/ges/session/GesSession.h"
#include "../support/Checks.h"
#include "../support/GlTest.h"
#include "project/command/ClipCommands.h"
#include "project/command/Command.h"
#include "project/command/MediaCommands.h"
#include "project/model/Media.h"
#include "project/Editor.h"
#include "render/Flatten.h"
#include "render/Resolve.h"

namespace {

using genesis::test::check;

using namespace genesis::project;
namespace render = genesis::render;
namespace engine = genesis::adapters::engine;
namespace ges_adapter = genesis::adapters::engine::ges;
using genesis::core::Rational;

// One frame at 30 fps, as a seek tolerance: ACCURATE seek lands on a frame
// boundary, so the sought point and the reported position may differ by up to
// one frame's worth of wall time without the session being wrong.
constexpr double kSeekToleranceSeconds = 1.0 / 30.0;

void sleep_ms(long long ms)
{
    std::this_thread::sleep_for(std::chrono::milliseconds(ms));
}

// The media file the asset request must actually load. Generated on demand as
// a one-second VP8/WebM (the only encoder guaranteed on this machine). Returns
// an empty path when generation fails, and the test then fails loudly rather
// than skip.
struct MediaFixture
{
    std::string path;
    bool owns_file = false;
};

MediaFixture media_fixture()
{
    const std::filesystem::path generated =
            std::filesystem::temp_directory_path() / "genesis-ges-session-fixture.webm";
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

ProjectFixture project_fixture(const std::string &media_path)
{
    ProjectFixture f;
    NewMedia item;
    item.path = media_path;
    item.name = "fixture.webm";
    item.duration = Rational{ 1, 1 };
    item.kind = MediaKind::Video;
    item.width = 640;
    item.height = 360;
    item.has_audio = false;
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

// The resolved graph the session compiles, from the shared media path.
render::BuiltTimeline built_timeline(const std::string &media_path)
{
    const ProjectFixture fixture = project_fixture(media_path);
    (void)fixture.clip_id;
    const std::vector<render::ExportClip> flat =
            render::flatten_timeline(fixture.editor.project(), std::nullopt);
    check(flat.size() == 1, "one clip flattens");
    render::ExportRequest request;
    request.width = 640;
    request.height = 360;
    return render::build_timeline(request, genesis::core::FrameRate::THIRTY,
                                  std::span<const render::ExportClip>(flat), { });
}

void test_load_prerolls_to_zero(const render::BuiltTimeline &built)
{
    ges_adapter::GesSession session;
    check(session.load(built), "load compiles and prerolls the graph");
    check(session.position() == Rational::ZERO, "prerolled playhead is zero");
}

void test_play_advances_position(const render::BuiltTimeline &built)
{
    ges_adapter::GesSession session;
    check(session.load(built), "load compiles and prerolls the graph");
    check(session.play(), "play starts");
    sleep_ms(500);
    const Rational position = session.position();
    check(position > Rational::ZERO, "position advances past zero while playing");
}

void test_pause_holds_and_seek_lands(const render::BuiltTimeline &built)
{
    ges_adapter::GesSession session;
    check(session.load(built), "load compiles and prerolls the graph");
    check(session.play(), "play starts");
    sleep_ms(300);

    check(session.pause(), "pause holds the frame");
    const Rational held = session.position();
    sleep_ms(200);
    check(session.position() == held, "position is frozen while paused");

    check(session.seek(Rational{ 1, 2 }), "seek to halfway");
    sleep_ms(300);
    const Rational landed = session.position();
    const double error = std::fabs(landed.as_double() - Rational{ 1, 2 }.as_double());
    check(error <= kSeekToleranceSeconds, "position lands within one frame of the sought point");
}

void test_reload_resets(const render::BuiltTimeline &built)
{
    ges_adapter::GesSession session;
    check(session.load(built), "first load compiles and prerolls");
    check(session.play(), "first load plays");
    sleep_ms(200);
    check(session.load(built), "reload tears down and compiles again");
    check(session.position() == Rational::ZERO, "reload resets the playhead to zero");
}

} // namespace

int main()
{
    gst_init(nullptr, nullptr);
    ges_init();

    const MediaFixture media = media_fixture();
    check(!media.path.empty(), "a media fixture is available");
    if (!media.path.empty()) {
        const render::BuiltTimeline built = built_timeline(media.path);
        check(built.timeline.clip_count() == 1, "one clip resolves");
        test_load_prerolls_to_zero(built);
        test_play_advances_position(built);
        test_pause_holds_and_seek_lands(built);
        test_reload_resets(built);
    }
    if (media.owns_file) {
        std::filesystem::remove(media.path);
    }

    return genesis::test::summary();
}
