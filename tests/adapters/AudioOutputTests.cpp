// SPDX-License-Identifier: GPL-3.0-or-later
// SPDX-FileCopyrightText: 2026 Genesis-0 contributors

#include <chrono>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <filesystem>
#include <mutex>
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

// The fixture's frame size, pinned to the same value as the generated WebM so
// the present frame's caps must match it exactly (see FrameTextureTests).
constexpr std::uint32_t kFrameWidth = 1280;
constexpr std::uint32_t kFrameHeight = 720;

void sleep_ms(long long ms)
{
    std::this_thread::sleep_for(std::chrono::milliseconds(ms));
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
            std::filesystem::temp_directory_path() / "genesis-audio-output-fixture.webm";
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

// Plays the session with the given audio setting and asserts the video frame
// path still delivers frames. The invariant under test: video plays whether or
// not an audio output could be opened, so the audio request must never take
// the picture down with it.
void run_play_session(const render::BuiltTimeline &built, bool audio_enabled, const char *label)
{
    // A headless EGL context, the way the spike proved it (see FrameTexture
    // Tests): GStreamer's GL elements do not make their own context.
    GstGLDisplay *display = reinterpret_cast<GstGLDisplay *>(gst_gl_display_egl_new());
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
        if (audio_enabled) {
            // The value is machine-dependent (no device -> false), so it is
            // reported rather than asserted; the invariant is that it returns
            // without crashing and video still plays either way.
            const bool opened = session.set_audio_output(true);
            std::printf("[%s] audio output opened: %s\n", label, opened ? "yes" : "no");
        } else {
            check(session.set_audio_output(false),
                  std::string(label) + ": disabling audio output succeeds");
        }
        check(session.set_gl_context(handles), "adopts the app's GL context");
        session.set_frame_sink(sink);
        check(session.load(built), std::string(label) + ": load compiles the graph");
        check(session.play(), std::string(label) + ": play starts");
        sleep_ms(500);

        {
            std::lock_guard<std::mutex> lock(log.mutex);
            check(log.count >= 1, std::string(label) + ": at least one frame arrives");
            check(log.texture_id != 0,
                  std::string(label) + ": frame carries a non-zero texture id");
            check(log.width == kFrameWidth && log.height == kFrameHeight,
                  std::string(label) + ": frame size matches the fixture (got "
                          + std::to_string(log.width) + "x" + std::to_string(log.height) + ")");
        }

        // Drop the keep_alive token the sink is still holding, then stop.
        {
            std::lock_guard<std::mutex> lock(log.mutex);
            log.held = engine::FrameTexture{ };
        }
        check(session.pause(), std::string(label) + ": pause holds");
    }

    // The wrapped context borrows these; release them only after the session
    // (and so the GL upload chain) is gone.
    gst_object_unref(context);
    gst_object_unref(display);
}

// Toggling the setting between loads must release the previous sink cleanly and
// rebuild either way. This drives the pure clock/graph path (no GL context), so
// it also covers a headless run with no adopted context.
void test_toggle_between_loads(const render::BuiltTimeline &built)
{
    ges_adapter::GesSession session;
    check(session.load(built), "load with audio off (default)");
    // Request audio: may open (and return true) or fall back (and return
    // false); either way the reload below must still work.
    const bool enabled = session.set_audio_output(true);
    std::printf("set_audio_output(true) -> %s\n", enabled ? "true" : "false");
    check(session.load(built), "reload with audio requested still works");
    check(session.set_audio_output(false), "disabling audio output succeeds");
    check(session.load(built), "reload with audio off still works");
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
        test_toggle_between_loads(built);
        run_play_session(built, true, "audio requested");
        run_play_session(built, false, "audio disabled");
    }
    if (media.owns_file) {
        std::filesystem::remove(media.path);
    }

    std::printf("%d checks, %d failures\n", checks, failures);
    return failures == 0 ? 0 : 1;
}
