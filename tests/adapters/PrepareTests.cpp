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

#include "adapters/engine/ges/session/GesSession.h"
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

// One frame at 30 fps, as a seek tolerance: ACCURATE seek lands on a frame
// boundary, so the sought point and the reported position may differ by up to
// one frame's worth of wall time without the session being wrong.
constexpr double kSeekToleranceSeconds = 1.0 / 30.0;

void sleep_ms(long long ms)
{
    std::this_thread::sleep_for(std::chrono::milliseconds(ms));
}

// Bounded wait tuned for a loaded machine: poll a readiness predicate every
// few milliseconds and give it seconds, so the suite waits for the engine to
// settle instead of falling through a fixed sleep and asserting too early.
constexpr auto kPollInterval = std::chrono::milliseconds(2);
constexpr auto kSettleTimeout = std::chrono::seconds(5);

template <typename Ready>
bool wait_until(Ready ready)
{
    const auto deadline = std::chrono::steady_clock::now() + kSettleTimeout;
    while (std::chrono::steady_clock::now() < deadline) {
        if (ready()) {
            return true;
        }
        sleep_ms(kPollInterval.count());
    }
    return false;
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
            std::filesystem::temp_directory_path() / "genesis-ges-prepare-fixture.webm";
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

// prepare is a pure state/seek operation: it warms the pipeline around the
// target so a later seek lands without a stall. It must not need a GL context
// or a frame sink, so the session is driven headlessly (no adopted context) the
// same way GesSessionTests drives load/play/pause/seek/position.

void test_prepare_before_load_returns_false()
{
    ges_adapter::GesSession session;
    check(!session.prepare(Rational::ZERO), "prepare before load returns false without crashing");
}

void test_prepare_paused_prerolls(const render::BuiltTimeline &built)
{
    ges_adapter::GesSession session;
    check(session.load(built), "load compiles and prerolls the graph");
    check(session.prepare(Rational::ZERO), "prepare(0) on a loaded, paused session prerolls");
    check(session.position() == Rational::ZERO, "prepare(0) leaves the playhead at zero");
}

void test_prepare_past_end_returns_false(const render::BuiltTimeline &built)
{
    ges_adapter::GesSession session;
    check(session.load(built), "load compiles and prerolls the graph");
    // The fixture timeline is exactly one second. A position past the end has
    // no frame to warm, so prepare returns false rather than clamping: the doc
    // comment promises "false when the position could not be warmed (an
    // unknown position...)", and a past-the-end instant is exactly an unknown
    // position - clamping would hide the unreachable target from the caller.
    check(!session.prepare(Rational{ 10, 1 }), "prepare past the end returns false");
}

void test_prepare_preserves_playing(const render::BuiltTimeline &built)
{
    ges_adapter::GesSession session;
    check(session.load(built), "load compiles and prerolls the graph");
    check(session.play(), "play starts");

    // Play must actually be rolling before prepare. Poll for the clock to move
    // rather than sleeping a fixed 200ms: a loaded machine may not have started
    // advancing yet, and prepare's was_playing detection depends on the state.
    check(wait_until([&] { return session.position() > Rational::ZERO; }),
          "play advances before prepare");

    const Rational midpoint = Rational{ 1, 2 };
    // One frame is the seek tolerance; `floor` is the lowest position that the
    // flush seek landing on the midpoint can legitimately report.
    const double floor = midpoint.as_double() - kSeekToleranceSeconds;

    const auto started = std::chrono::steady_clock::now();
    check(session.prepare(midpoint), "prepare(midpoint) warms while playing");

    // The session must still be rolling, not frozen where prepare left it: the
    // playhead is placed near the prepared point and keeps advancing.
    //
    // prepare's flush seek is not observable through the first position() query
    // after it returns - the query can still report the pre-seek position - and
    // while playing the playhead then keeps moving. So poll until the warmed
    // position surfaces, then bound how far it may have advanced by the wall
    // time since the call began: at 1x playback the playhead cannot outrun real
    // time, so this admits only the legitimate real-time drift and still rejects
    // a seek that overshot the prepared point. A single sample races both the
    // seek settling and the streaming clock under load.
    std::optional<Rational> placed;
    const bool warmed = wait_until([&] {
        const Rational now = session.position();
        if (now.as_double() >= floor) {
            placed = now;
            return true;
        }
        return false;
    });
    const double elapsed =
            std::chrono::duration<double>(std::chrono::steady_clock::now() - started).count();
    check(warmed && placed.has_value()
                  && placed->as_double() - midpoint.as_double() <= kSeekToleranceSeconds + elapsed,
          "playhead placed near the prepared midpoint");

    const Rational after = placed.value_or(session.position());
    check(wait_until([&] { return session.position() > after; }),
          "position continues to advance after prepare");
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
        test_prepare_before_load_returns_false();
        test_prepare_paused_prerolls(built);
        test_prepare_past_end_returns_false(built);
        test_prepare_preserves_playing(built);
    }
    if (media.owns_file) {
        std::filesystem::remove(media.path);
    }

    return genesis::test::summary();
}
