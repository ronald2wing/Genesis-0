// SPDX-License-Identifier: GPL-3.0-or-later
// SPDX-FileCopyrightText: 2026 Genesis-0 contributors

#include <chrono>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <filesystem>
#include <mutex>
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
#include "project/command/ClipCommands.h"
#include "project/command/Command.h"
#include "project/command/MediaCommands.h"
#include "project/model/Media.h"
#include "project/Editor.h"
#include "render/Flatten.h"
#include "render/Resolve.h"
#include "../support/Checks.h"
#include "../support/GlTest.h"

namespace {

using genesis::test::check;

using namespace genesis::project;
namespace render = genesis::render;
namespace ges_adapter = genesis::adapters::engine::ges;
namespace engine = genesis::adapters::engine;
using genesis::core::Rational;

// The fixture's frame size. The GES adapter does not yet apply the export
// request's size to the video track, so GES renders at its default
// restriction caps (1280x720); the generated WebM and the assertions both pin
// that same size so the present frame's caps must match it exactly.
constexpr std::uint32_t kFrameWidth = 1280;
constexpr std::uint32_t kFrameHeight = 720;

void sleep_ms(long long ms)
{
    std::this_thread::sleep_for(std::chrono::milliseconds(ms));
}

// Bounded wait for the first presented frame. A fixed sleep is the one
// wall-clock assumption that flaked on CI: the GL upload of the first
// 1280x720 frame runs on llvmpipe (software) under a shared, -j4-loaded
// runner, so the picture can lag a scheduler hiccup. Poll instead, with a
// deadline comfortably past the session's own 5s preroll budget, so the test
// waits for the engine rather than asserting too early.
constexpr auto kPollInterval = std::chrono::milliseconds(5);
constexpr auto kFrameTimeout = std::chrono::seconds(10);

template <typename Ready>
bool wait_until(Ready ready)
{
    const auto deadline = std::chrono::steady_clock::now() + kFrameTimeout;
    while (std::chrono::steady_clock::now() < deadline) {
        if (ready()) {
            return true;
        }
        sleep_ms(kPollInterval.count());
    }
    return false;
}

// The media file the asset request must actually load. Generated on demand as
// a one-second VP8/WebM at a fixed size (the only encoder guaranteed on this
// machine). Returns an empty path when generation fails, and the test then
// fails loudly rather than skip.
struct MediaFixture
{
    std::string path;
    bool owns_file = false;
};

MediaFixture media_fixture()
{
    const std::filesystem::path generated =
            std::filesystem::temp_directory_path() / "genesis-frame-texture-fixture.webm";
    std::filesystem::remove(generated);
    // 30 frames of testsrc at 30 fps is exactly one second, pinned to the
    // size the assertion expects.
    const std::string command = "gst-launch-1.0 -q videotestsrc num-buffers=30 ! "
                                "video/x-raw,width=1280,height=720,framerate=30/1 ! videoconvert ! "
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
    item.width = kFrameWidth;
    item.height = kFrameHeight;
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
    request.width = kFrameWidth;
    request.height = kFrameHeight;
    return render::build_timeline(request, genesis::core::FrameRate::THIRTY,
                                  std::span<const render::ExportClip>(flat), { });
}

// What the frame sink records. Written by the session's streaming thread and
// read back by the test, so every field sits behind the mutex.
struct FrameLog
{
    std::mutex mutex;
    int count = 0;
    std::uint64_t texture_id = 0;
    std::uint32_t width = 0;
    std::uint32_t height = 0;
    // Holds one frame's keep_alive token so the test can drop it explicitly.
    engine::FrameTexture held;
};

void test_presents_frames(const render::BuiltTimeline &built)
{
    // A headless EGL context, the way the spike proved it: GStreamer's GL
    // elements do not make their own context, so the test supplies one and
    // hands its raw handles across the seam.
    GstGLDisplay *display = genesis::test::make_egl_display();
    check(display != nullptr, "headless EGL display is available");
    if (display == nullptr) {
        return;
    }
    GstGLContext *context = gst_gl_context_new(display);
    GError *error = nullptr;
    const gboolean created = gst_gl_context_create(context, nullptr, &error);
    if (!created) {
        check(false,
              std::string("headless GL context creates: ")
                      + (error != nullptr ? error->message : "unknown"));
        g_clear_error(&error);
        gst_object_unref(context);
        gst_object_unref(display);
        return;
    }

    engine::GlHandles handles;
    handles.display = gst_gl_display_get_handle(display);
    handles.context = gst_gl_context_get_gl_context(context);

    FrameLog log;
    engine::FrameSink sink = [&log](engine::FrameTexture frame) {
        std::lock_guard<std::mutex> lock(log.mutex);
        ++log.count;
        if (frame.texture_id != 0) {
            log.texture_id = frame.texture_id;
            log.width = frame.width;
            log.height = frame.height;
        }
        log.held = std::move(frame);
    };

    {
        ges_adapter::GesSession session;
        check(session.set_gl_context(handles), "adopts the app's GL context");
        session.set_frame_sink(sink);
        check(session.load(built), "load compiles the GL presentation graph");
        check(session.play(), "play starts");

        const bool arrived = wait_until([&log] {
            std::lock_guard<std::mutex> lock(log.mutex);
            return log.count >= 1;
        });

        {
            std::lock_guard<std::mutex> lock(log.mutex);
            check(arrived, "at least one frame arrives");
            check(log.texture_id != 0, "frame carries a non-zero texture id");
            check(log.width == kFrameWidth && log.height == kFrameHeight,
                  "frame size matches the fixture (got " + std::to_string(log.width) + "x"
                          + std::to_string(log.height) + ")");
        }

        // Drop the keep_alive token the sink is still holding, then stop: the
        // token's deleter must release the buffer and the pipeline must tear
        // down without touching the recycled frame.
        {
            std::lock_guard<std::mutex> lock(log.mutex);
            log.held = engine::FrameTexture{ };
        }
        check(session.pause(), "pause after dropping the token");
    }

    // The wrapped context borrows these; release them only after the session
    // (and so the GL upload chain) is gone.
    gst_object_unref(context);
    gst_object_unref(display);
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
        test_presents_frames(built);
    }
    if (media.owns_file) {
        std::filesystem::remove(media.path);
    }

    return genesis::test::summary();
}
