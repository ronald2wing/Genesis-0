// SPDX-License-Identifier: GPL-3.0-or-later
// SPDX-FileCopyrightText: 2026 Genesis-0 contributors
//
// The media bin controller's suite: drives the pure filter/sort, the id ->
// cache-path mapping, the waveform decode, and the off-thread poster/filmstrip
// generation headlessly. The filter/sort needs no engine (a hand-built list);
// the generation test does: it generates a one-second VP8/WebM source and
// drives a real GES poster + filmstrip through the controller, proving the
// generation leaves the UI thread (R4), writes both caches, and stays fresh on
// a re-run.

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
#include <QVariantMap>

#include "../support/Wait.h"

#include "app/controllers/MediaBinController.h"
#include "project/command/Command.h"
#include "project/command/MediaCommands.h"
#include "project/model/Media.h"
#include "project/Editor.h"
#include "render/MediaCache.h"
#include "render/Peaks.h"

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

// A media map in the TimelineModel projection's shape, for the pure apply()
// tests - the projection itself is TimelineModel's, so the suite feeds the
// same { id, name, path, duration } maps apply() is asked to sort.
QVariantMap media_map(const char *id, const char *name, const char *path, double duration)
{
    QVariantMap m;
    m.insert(QStringLiteral("id"), QString::fromLatin1(id));
    m.insert(QStringLiteral("name"), QString::fromLatin1(name));
    m.insert(QStringLiteral("path"), QString::fromLatin1(path));
    m.insert(QStringLiteral("duration"), duration);
    return m;
}

void test_apply_filters_and_sorts()
{
    MediaBinController controller;
    QVariantList items;
    items.append(media_map("m1", "Alpha.webm", "/a/Alpha.webm", 5.0));
    items.append(media_map("m2", "beta.jpg", "/a/beta.jpg", -1.0));
    items.append(media_map("m3", "gamma.mp3", "/a/gamma.mp3", 30.0));

    // The query matches name or path, case-insensitively.
    const QVariantList byQuery = controller.apply(items, QStringLiteral("BETA"), 0);
    check(byQuery.size() == 1, "the query matches one item");
    check(byQuery[0].toMap().value(QStringLiteral("id")).toString() == QStringLiteral("m2"),
          "the query matched by name");

    // Name sort is case-insensitive: alpha < beta < gamma.
    const QVariantList byName = controller.apply(items, QString(), 0);
    check(byName.size() == 3, "no query keeps every item");
    check(byName[0].toMap().value(QStringLiteral("id")).toString() == QStringLiteral("m1"),
          "name sort puts Alpha first");
    check(byName[2].toMap().value(QStringLiteral("id")).toString() == QStringLiteral("m3"),
          "name sort puts gamma last");

    // Duration sort is longest first, unknown (-1) last.
    const QVariantList byDuration = controller.apply(items, QString(), 1);
    check(byDuration[0].toMap().value(QStringLiteral("id")).toString() == QStringLiteral("m3"),
          "duration sort puts 30s first");
    check(byDuration[1].toMap().value(QStringLiteral("id")).toString() == QStringLiteral("m1"),
          "duration sort puts 5s second");
    check(byDuration[2].toMap().value(QStringLiteral("id")).toString() == QStringLiteral("m2"),
          "unknown duration sinks last");

    // Import order is the projection's own order, untouched.
    const QVariantList byImport = controller.apply(items, QString(), 2);
    check(byImport[0].toMap().value(QStringLiteral("id")).toString() == QStringLiteral("m1"),
          "import order keeps the first item");
    check(byImport[2].toMap().value(QStringLiteral("id")).toString() == QStringLiteral("m3"),
          "import order keeps the last item");
}

// A media item's id, as the editor mints it.
std::string add_media(Editor &editor, NewMedia item)
{
    const auto outcome = editor.apply(Command{ AddMedia{ std::move(item) } });
    check(outcome.has_value() && outcome->created_id.has_value(), "AddMedia mints an id");
    return outcome->created_id ? *outcome->created_id : std::string{ };
}

void test_poster_source_by_kind()
{
    MediaBinController controller;
    controller.setCacheRoot(QStringLiteral("/tmp/genesis-mediabin-cache"));

    Editor editor;
    NewMedia image;
    image.path = "/footage/still.png";
    image.name = "still.png";
    image.kind = MediaKind::Image;
    image.width = 100;
    image.height = 100;
    const std::string image_id = add_media(editor, std::move(image));

    NewMedia video;
    video.path = "/footage/clip.webm";
    video.name = "clip.webm";
    video.kind = MediaKind::Video;
    video.width = 640;
    video.height = 360;
    const std::string video_id = add_media(editor, std::move(video));

    controller.setItems(editor.project());

    // An image card draws the source itself, whatever caches exist.
    check(controller.posterSource(QString::fromStdString(image_id), 0)
                  == QStringLiteral("file:///footage/still.png"),
          "an image card shows the source itself");

    // A video card has no poster until generation fills it.
    check(controller.posterSource(QString::fromStdString(video_id), 0).isEmpty(),
          "a video card is empty before generation");

    // An unknown id reads empty rather than crashing.
    check(controller.posterSource(QStringLiteral("nope"), 0).isEmpty(),
          "an unknown id reads empty");
}

void test_waveform_reads_existing_peaks()
{
    MediaBinController controller;
    const std::filesystem::path root = "/tmp/genesis-mediabin-cache";
    controller.setCacheRoot(QString::fromStdString(root.string()));

    Editor editor;
    NewMedia audio;
    audio.path = "/footage/sound.wav";
    audio.name = "sound.wav";
    audio.kind = MediaKind::Audio;
    audio.has_audio = true;
    const std::string audio_id = add_media(editor, std::move(audio));
    controller.setItems(editor.project());

    // No cache yet: empty waveform.
    check(controller.waveformPeaks(QString::fromStdString(audio_id), 0).isEmpty(),
          "no peaks before a cache exists");

    // Write a valid .peaks cache at the mapped path, then read it back.
    const genesis::render::CachePaths paths =
            genesis::render::cache_paths(root, std::filesystem::path("/footage/sound.wav"));
    genesis::render::Peaks peaks;
    peaks.buckets_per_second = 40.0f;
    for (int i = 0; i < 40; ++i) {
        peaks.min.push_back(-(i + 1) * 0.01f);
        peaks.max.push_back((i + 1) * 0.01f);
    }
    const std::vector<std::uint8_t> bytes = peaks.encode();
    std::filesystem::create_directories(paths.waveform.parent_path());
    {
        std::ofstream out(paths.waveform, std::ios::binary);
        out.write(reinterpret_cast<const char *>(bytes.data()),
                  static_cast<std::streamsize>(bytes.size()));
    }

    const QVariantList read = controller.waveformPeaks(QString::fromStdString(audio_id), 0);
    check(read.size() == 40, "the waveform decodes to its bucket count");
    check(read[0].toMap().value(QStringLiteral("min")).toDouble() < 0.0,
          "a bucket carries its negative minimum");
    check(read[39].toMap().value(QStringLiteral("max")).toDouble() > 0.0,
          "a bucket carries its positive maximum");

    // A non-audio id has no waveform, cache or not.
    check(controller.waveformPeaks(QStringLiteral("nope"), 0).isEmpty(),
          "an unknown id has no waveform");
}

// The missing-media recovery invokables, driven through a real Editor so the
// one-undo-step contract is proven alongside the state change. relinkMedia and
// setMediaPlaceholder only write the model, so they need no file on disk;
// fillSlot probes the generated fixture, so it takes the fixture's path.

void test_placeholder_flag_set_and_clear()
{
    Editor editor;
    MediaBinController controller;
    controller.setEditor(&editor);

    NewMedia media;
    media.path = "/template/slot.webm";
    media.name = "slot.webm";
    media.kind = MediaKind::Video;
    const std::string id = add_media(editor, std::move(media));

    check(!controller.isPlaceholder(QString::fromStdString(id)), "a fresh import is not a slot");

    check(controller.setMediaPlaceholder(QString::fromStdString(id), true),
          "marking a slot applies");
    check(controller.isPlaceholder(QString::fromStdString(id)), "the item is now a slot");
    check(!controller.setMediaPlaceholder(QString::fromStdString(id), true),
          "marking an already-marked slot is a no-op");

    editor.undo();
    check(!controller.isPlaceholder(QString::fromStdString(id)), "one undo clears the slot flag");
    check(editor.project().media_by_id(id) != nullptr, "the media survives the slot-flag undo");

    check(controller.setMediaPlaceholder(QString::fromStdString(id), true), "re-marking applies");
    check(controller.setMediaPlaceholder(QString::fromStdString(id), false),
          "clearing the slot applies");
    check(!controller.isPlaceholder(QString::fromStdString(id)),
          "the item is ordinary media again");
    editor.undo();
    check(controller.isPlaceholder(QString::fromStdString(id)),
          "one undo returns the flag to marked");
}

void test_relink_media_updates_path()
{
    Editor editor;
    MediaBinController controller;
    controller.setEditor(&editor);

    NewMedia media;
    media.path = "/footage/clip.webm";
    media.name = "clip.webm";
    media.kind = MediaKind::Video;
    const std::string id = add_media(editor, std::move(media));

    // A file:// URL normalises to a plain local path.
    check(controller.relinkMedia(QString::fromStdString(id),
                                 QStringLiteral("file:///new/relinked.webm")),
          "relinking through a file URL applies");
    check(editor.project().media_by_id(id)->path == "/new/relinked.webm",
          "the path becomes the URL's local path");

    editor.undo();
    check(editor.project().media_by_id(id)->path == "/footage/clip.webm",
          "one undo restores the old path");

    // Relinking to the path it already has is a tolerated no-op.
    check(!controller.relinkMedia(QString::fromStdString(id), QStringLiteral("/footage/clip.webm")),
          "relinking to the same path is a no-op");
}

void test_fill_slot_swaps_identity(const std::string &media_path)
{
    Editor editor;
    MediaBinController controller;
    controller.setEditor(&editor);

    NewMedia slot;
    slot.path = "/template/slot.webm";
    slot.name = "slot.webm";
    slot.kind = MediaKind::Video;
    const std::string id = add_media(editor, std::move(slot));
    controller.setMediaPlaceholder(QString::fromStdString(id), true);

    check(controller.fillSlot(QString::fromStdString(id), QString::fromStdString(media_path)),
          "filling a slot applies");
    const genesis::project::MediaItem *filled = editor.project().media_by_id(id);
    check(filled != nullptr, "the slot keeps its id");
    check(filled->path == media_path, "the slot takes the new file's path");
    check(filled->kind == MediaKind::Video, "the slot takes the new kind");
    check(!filled->placeholder, "a filled slot is ordinary media");
    check(!controller.isPlaceholder(QString::fromStdString(id)),
          "isPlaceholder clears after the fill");

    editor.undo();
    const genesis::project::MediaItem *reverted = editor.project().media_by_id(id);
    check(reverted != nullptr && reverted->placeholder,
          "one undo returns the slot to a placeholder");
    check(reverted->path == "/template/slot.webm", "one undo restores the template's path");
}

// A one-second VP8/WebM source (the encoder guaranteed on this machine),
// generated on demand. An empty path fails the generation test loudly.
std::string media_fixture()
{
    const std::filesystem::path generated =
            std::filesystem::temp_directory_path() / "genesis-mediabin-fixture.webm";
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

void test_generation_runs_off_thread(const std::string &media_path)
{
    MediaBinController controller;
    const std::filesystem::path root =
            std::filesystem::temp_directory_path() / "genesis-mediabin-cache";
    std::filesystem::remove_all(root);
    controller.setCacheRoot(QString::fromStdString(root.string()));

    Editor editor;
    NewMedia video;
    video.path = media_path;
    video.name = "fixture.webm";
    video.kind = MediaKind::Video;
    const std::string video_id = add_media(editor, std::move(video));
    controller.setItems(editor.project());

    controller.generateMissingCaches();
    check(controller.generating(), "generation starts");
    const bool done = wait_until_idle(controller, [&] { return !controller.generating(); });
    check(done, "generation finishes within the timeout");
    check(!controller.generating(), "generation is no longer running");
    check(controller.progress() == controller.total(), "progress reaches the total job count");
    check(controller.cacheVersion() > 0, "the cache version is bumped");

    const quintptr worker = controller.workerThreadId();
    check(worker != 0, "the worker thread id is recorded");
    check(worker != reinterpret_cast<quintptr>(QThread::currentThread()),
          "generation left the controller's thread");

    // Both caches land at the mapped paths and read back through posterSource.
    const genesis::render::CachePaths paths =
            genesis::render::cache_paths(root, std::filesystem::path(media_path));
    check(std::filesystem::exists(paths.poster) && std::filesystem::file_size(paths.poster) > 0,
          "the poster is written");
    check(std::filesystem::exists(paths.filmstrip)
                  && std::filesystem::file_size(paths.filmstrip) > 0,
          "the filmstrip is written");
    check(controller.posterSource(QString::fromStdString(video_id), controller.cacheVersion())
                  .startsWith(QStringLiteral("file://")),
          "posterSource reports the generated poster after the run");

    // A re-run finds both caches fresh: it finishes without rewriting them.
    const std::uintmax_t poster_size = std::filesystem::file_size(paths.poster);
    controller.generateMissingCaches();
    check(wait_until_idle(controller, [&] { return !controller.generating(); }),
          "a re-run finishes");
    check(std::filesystem::file_size(paths.poster) == poster_size,
          "a fresh cache is not regenerated");

    std::filesystem::remove_all(root);
}

} // namespace

int main(int argc, char **argv)
{
    QCoreApplication app(argc, argv);
    gst_init(&argc, &argv);
    ges_init();

    test_apply_filters_and_sorts();
    test_poster_source_by_kind();
    test_waveform_reads_existing_peaks();
    test_placeholder_flag_set_and_clear();
    test_relink_media_updates_path();

    const std::string media = media_fixture();
    check(!media.empty(), "a media fixture is available");
    if (!media.empty()) {
        test_generation_runs_off_thread(media);
        test_fill_slot_swaps_identity(media);
        std::filesystem::remove(media);
    }

    std::printf("%d checks, %d failures\n", checks, failures);
    return failures == 0 ? 0 : 1;
}
