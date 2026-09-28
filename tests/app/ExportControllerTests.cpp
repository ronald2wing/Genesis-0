// SPDX-License-Identifier: GPL-3.0-or-later
// SPDX-FileCopyrightText: 2026 Genesis-0 contributors
//
// The export controller's suite: drives the form -> validate -> worker ->
// outcome mapping headlessly. Validation needs no engine (the project fixture
// has a fake media path; validate never touches the file), but the two run
// tests do: they generate a one-second VP8/WebM source and drive a real GES
// render through the controller, proving the render leaves the UI thread (R4),
// reports completion, and a cancel leaves nothing behind.

#include <cstdio>
#include <cstdlib>
#include <filesystem>
#include <fstream>
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

#include "app/controllers/ExportController.h"
#include "project/command/ClipCommands.h"
#include "project/command/Command.h"
#include "project/command/MediaCommands.h"
#include "project/model/Media.h"
#include "project/Editor.h"
#include "../support/Checks.h"

namespace {

using genesis::test::check;

using namespace genesis::project;
using genesis::core::Rational;
using genesis::test::wait_until_idle;

// A project with one video media item and one clip on the first track. The
// media path is fake by default - validate never opens it - and the run tests
// pass the real generated fixture.
struct Fixture
{
    Editor editor;
    std::string media_id;
    std::string clip_id;
};

Fixture fixture(const std::string &path = "/footage/a.webm")
{
    Fixture f;
    NewMedia item;
    item.path = path;
    item.name = "a.webm";
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
    return f;
}

// One second at 30 fps, the tolerance for a duration reading.
constexpr double kFrameSeconds = 1.0 / 30.0;

// A one-second VP8/WebM source (the encoder guaranteed on this machine),
// generated on demand. An empty path fails the run tests loudly.
std::string media_fixture()
{
    const std::filesystem::path generated =
            std::filesystem::temp_directory_path() / "genesis-export-controller-fixture.webm";
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

void test_validate_reports_each_problem()
{
    Fixture f = fixture();
    ExportController controller(&f.editor);

    check(controller.validate().isEmpty(), "defaults validate over one clip");

    controller.setOutputPath(QString());
    check(controller.validate().contains(QStringLiteral("no output path")),
          "an empty path is a problem");
    controller.setOutputPath(QStringLiteral("out.webm"));

    controller.setWidth(0);
    check(controller.validate().contains(QStringLiteral("frame width is zero")),
          "a zero width is a problem");
    controller.setWidth(1920);

    controller.setHeight(0);
    check(controller.validate().contains(QStringLiteral("frame height is zero")),
          "a zero height is a problem");
    controller.setHeight(1080);

    controller.setRateNum(0);
    check(controller.validate().contains(QStringLiteral("frame rate is not positive")),
          "a zero rate numerator is a problem");
    controller.setRateNum(30);
    controller.setRateDen(0);
    check(controller.validate().contains(QStringLiteral("frame rate is not positive")),
          "a zero rate denominator is a problem");
    controller.setRateDen(1);

    check(controller.validate().isEmpty(), "the form validates again once restored");
}

void test_presets_are_exposed_and_resolve()
{
    Fixture f = fixture();
    ExportController controller(&f.editor);

    check(controller.presets().size() >= 5, "the preset list exposes several targets");
    check(controller.presets().contains(QStringLiteral("vp8")), "vp8 is offered");
    check(controller.presets().contains(QStringLiteral("h264")), "h264 is offered");
    check(controller.presets().contains(QStringLiteral("av1")), "av1 is offered");

    check(controller.preset() == QStringLiteral("vp8"),
          "the default preset is vp8 (the guaranteed encoder)");
    check(controller.quality() == QStringLiteral("3 Mb/s"),
          "vp8's quality hint resolves from the catalogue");

    controller.setPreset(QStringLiteral("h264"));
    check(controller.preset() == QStringLiteral("h264"), "the preset changes");
    check(controller.quality() == QStringLiteral("23 crf"), "h264's quality hint resolves");

    controller.setPreset(QStringLiteral("av1"));
    check(controller.quality() == QStringLiteral("38 qp"), "av1's quality hint resolves");
}

void test_empty_project_is_invalid()
{
    Editor editor; // a fresh project with no clips
    ExportController controller(&editor);
    check(controller.validate().contains(QStringLiteral("timeline has no clips")),
          "a clip-less timeline is a problem");
}

void test_start_export_refuses_when_invalid()
{
    Fixture f = fixture();
    ExportController controller(&f.editor);
    controller.setWidth(0);
    controller.startExport();
    check(!controller.running(), "an invalid form does not start a run");
    check(!controller.error().isEmpty(), "the refusal leaves a message behind");
}

void test_export_runs_off_thread_and_writes_file(const std::string &media_path)
{
    Fixture f = fixture(media_path);
    ExportController controller(&f.editor);
    const std::filesystem::path output =
            std::filesystem::temp_directory_path() / "genesis-export-controller-out.webm";
    std::filesystem::remove(output);

    bool finished_ok = false;
    QString finished_error;
    QObject::connect(&controller, &ExportController::finished,
                     [&](bool ok, const QString &error, double) {
                         finished_ok = ok;
                         finished_error = error;
                     });

    controller.setOutputPath(QString::fromStdString(output.string()));
    controller.setWidth(320);
    controller.setHeight(180);
    controller.startExport();

    check(controller.running(), "the run starts");
    const bool done = wait_until_idle(controller, [&] { return !controller.running(); });
    check(done, "the run finishes within the timeout");
    check(controller.status() == QStringLiteral("Done"), "status is Done");
    check(controller.error().isEmpty(), "no error on success");
    check(controller.progress() >= 1.0, "progress reaches completion");
    check(std::filesystem::exists(output) && std::filesystem::file_size(output) > 0,
          "the output file is written");

    const quintptr worker = controller.workerThreadId();
    check(worker != 0, "the worker thread id is recorded");
    check(worker != reinterpret_cast<quintptr>(QThread::currentThread()),
          "the render left the controller's thread");

    check(finished_ok, "finished reports success");
    check(finished_error.isEmpty(), "a success has no error");

    std::filesystem::remove(output);
}

void test_preset_drives_the_codec(const std::string &media_path)
{
    Fixture f = fixture(media_path);
    ExportController controller(&f.editor);
    const std::filesystem::path output =
            std::filesystem::temp_directory_path() / "genesis-export-controller-h264.mov";
    std::filesystem::remove(output);

    controller.setOutputPath(QString::fromStdString(output.string()));
    controller.setPreset(QStringLiteral("h264"));
    controller.setWidth(320);
    controller.setHeight(180);
    controller.startExport();

    const bool done = wait_until_idle(controller, [&] { return !controller.running(); });
    check(done, "the h264 run finishes within the timeout");

    if (controller.error().isEmpty()) {
        // H.264 exports as a QuickTime container (see CodecPlan): the file is
        // ISO BMFF, whose leading atom is a 4-byte size followed by "ftyp".
        // A WebM (vp8/vp9/av1) would instead open with the EBML magic.
        std::ifstream in(output, std::ios::binary);
        char magic[8] = { };
        in.read(magic, 8);
        check(std::string(magic + 4, 4) == "ftyp", "the h264 preset wrote a QuickTime container");
    } else {
        // The host may lack openh264enc; the refusal still proves the preset
        // reached the engine (it names the h264 encoder, not a vp8 fallback).
        check(controller.error().contains(QStringLiteral("openh264")),
              "a missing h264 encoder is reported, not silently remapped");
    }

    std::filesystem::remove(output);
}

void test_cancel_leaves_no_file(const std::string &media_path)
{
    Fixture f = fixture(media_path);
    ExportController controller(&f.editor);
    const std::filesystem::path output =
            std::filesystem::temp_directory_path() / "genesis-export-controller-cancel.webm";
    std::filesystem::remove(output);

    controller.setOutputPath(QString::fromStdString(output.string()));
    controller.setWidth(320);
    controller.setHeight(180);

    controller.startExport();
    // The flag is set before the worker's first poll, so the render unwinds
    // without ever writing the file.
    controller.cancelExport();

    const bool done = wait_until_idle(controller, [&] { return !controller.running(); });
    check(done, "the cancel lands within the timeout");
    check(controller.status() == QStringLiteral("Cancelled"), "the run reports itself cancelled");
    check(!std::filesystem::exists(output), "no partial file is left behind");
}

} // namespace

int main(int argc, char **argv)
{
    QCoreApplication app(argc, argv);
    gst_init(&argc, &argv);
    ges_init();

    test_validate_reports_each_problem();
    test_empty_project_is_invalid();
    test_start_export_refuses_when_invalid();
    test_presets_are_exposed_and_resolve();

    const std::string media = media_fixture();
    check(!media.empty(), "a media fixture is available");
    if (!media.empty()) {
        test_export_runs_off_thread_and_writes_file(media);
        test_cancel_leaves_no_file(media);
        test_preset_drives_the_codec(media);
        std::filesystem::remove(media);
    }

    return genesis::test::summary();
}
