// SPDX-License-Identifier: GPL-3.0-or-later
// SPDX-FileCopyrightText: 2026 Genesis-0 contributors
//
// The reverse controller's suite: drives the off-thread generation of reversed
// copies headlessly. The generation needs a real source, so the suite makes a
// one-second VP8/WebM and drives it through a reversed clip, proving the
// generation leaves the UI thread (R4), writes the reverse cache, fires
// `finished`, and stays fresh on a re-run.

#include <cstdio>
#include <cstdlib>
#include <filesystem>
#include <string>

// GStreamer/GES pull in glib, whose gdbusintrospection.h has a field named
// `signals` - a Qt keyword macro. Include the glib headers first so the field
// is parsed before Qt defines the macro (see main.cpp).
#include <ges/ges.h>
#include <gst/gst.h>

#include <QCoreApplication>
#include <QString>
#include <QThread>

#include "../support/Wait.h"

#include "app/controllers/ReverseController.h"
#include "project/command/ClipCommands.h"
#include "project/command/Command.h"
#include "project/command/MediaCommands.h"
#include "project/command/PropertyCommands.h"
#include "project/model/Media.h"
#include "project/Editor.h"
#include "render/ReverseCache.h"

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
using genesis::core::Rational;
using genesis::test::wait_until_idle;

// A project with one video media item and one clip on the first track, marked
// reversed through the real command layer.
struct Fixture
{
    Editor editor;
    std::string media_id;
    std::string clip_id;
};

Fixture fixture(const std::string &path)
{
    Fixture f;
    NewMedia item;
    item.path = path;
    item.name = "fixture.webm";
    item.duration = Rational{ 1, 1 };
    item.kind = MediaKind::Video;
    item.width = 640;
    item.height = 360;
    item.has_audio = false;
    const auto add_media = f.editor.apply(Command{ AddMedia{ std::move(item) } });
    check(add_media.has_value() && add_media->created_id.has_value(), "adds media");
    f.media_id = *add_media->created_id;
    const std::string track_id = f.editor.project().timelines[0].tracks[0].id;
    const auto add_clip =
            f.editor.apply(Command{ AddClip{ f.media_id, track_id, Rational{ 0, 1 }, false } });
    check(add_clip.has_value() && add_clip->created_id.has_value(), "adds a clip");
    f.clip_id = *add_clip->created_id;
    const auto reverse = f.editor.apply(Command{ SetClipReverse{ f.clip_id, true } });
    check(reverse.has_value(), "marks the clip reversed");
    return f;
}

// A one-second VP8/WebM source (the encoder guaranteed on this machine),
// generated on demand. An empty path fails the generation test loudly.
std::string media_fixture()
{
    const std::filesystem::path generated =
            std::filesystem::temp_directory_path() / "genesis-reverse-controller-fixture.webm";
    std::filesystem::remove(generated);
    const std::string command = "gst-launch-1.0 -q videotestsrc num-buffers=30 ! videoconvert ! "
                                "vp8enc ! webmmux ! filesink location=\""
            + generated.string() + "\" >/dev/null 2>&1";
    if (std::system(command.c_str()) == 0 && std::filesystem::exists(generated)
        && std::filesystem::file_size(generated) > 0) {
        return generated.string();
    }
    std::filesystem::remove(generated);
    return { };
}

void test_an_empty_cache_root_starts_nothing()
{
    ReverseController controller;
    Editor editor;

    controller.generateMissingReverses(editor.project(), std::filesystem::path());
    check(!controller.generating(), "an empty cache root starts no generation");
    check(controller.workerThreadId() == 0, "an empty cache root records no worker");
}

void test_generation_runs_off_thread_and_writes_reverse(const std::string &media_path)
{
    ReverseController controller;
    const std::filesystem::path root =
            std::filesystem::temp_directory_path() / "genesis-reverse-controller-cache";
    std::filesystem::remove_all(root);

    Fixture f = fixture(media_path);

    bool finished = false;
    QObject::connect(&controller, &ReverseController::finished, [&finished]() { finished = true; });

    controller.generateMissingReverses(f.editor.project(), root);
    check(controller.generating(), "generation starts");
    const bool done = wait_until_idle(controller, [&] { return !controller.generating(); });
    check(done, "generation finishes within the timeout");
    check(!controller.generating(), "generation is no longer running");
    check(finished, "finished fires on the controller's thread");

    const quintptr worker = controller.workerThreadId();
    check(worker != 0, "the worker thread id is recorded");
    check(worker != reinterpret_cast<quintptr>(QThread::currentThread()),
          "generation left the controller's thread");

    const std::filesystem::path reverse_file =
            genesis::render::reverse_cache_path(root, std::filesystem::path(media_path), 0.0, 1.0);
    check(std::filesystem::exists(reverse_file) && std::filesystem::file_size(reverse_file) > 0,
          "the reverse copy is written");

    // A re-run finds the copy fresh: it finishes without rewriting it.
    const auto first_mtime = std::filesystem::last_write_time(reverse_file);
    controller.generateMissingReverses(f.editor.project(), root);
    check(wait_until_idle(controller, [&] { return !controller.generating(); }),
          "a re-run finishes");
    check(std::filesystem::last_write_time(reverse_file) == first_mtime,
          "a fresh reverse is not regenerated");

    std::filesystem::remove_all(root);
}

} // namespace

int main(int argc, char **argv)
{
    QCoreApplication app(argc, argv);
    gst_init(&argc, &argv);
    ges_init();

    test_an_empty_cache_root_starts_nothing();

    const std::string media = media_fixture();
    check(!media.empty(), "a media fixture is available");
    if (!media.empty()) {
        test_generation_runs_off_thread_and_writes_reverse(media);
        std::filesystem::remove(media);
    }

    std::printf("%d checks, %d failures\n", checks, failures);
    return failures == 0 ? 0 : 1;
}
