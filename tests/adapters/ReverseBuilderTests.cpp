// SPDX-License-Identifier: GPL-3.0-or-later
// SPDX-FileCopyrightText: 2026 Genesis-0 contributors

#include <cstdio>
#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <optional>
#include <span>
#include <string>
#include <vector>

#include <ges/ges.h>
#include <gst/gst.h>

#include "adapters/engine/ges/media/Reverse.h"
#include "adapters/engine/ges/session/GesBuilder.h"
#include "project/command/ClipCommands.h"
#include "project/command/Command.h"
#include "project/command/MediaCommands.h"
#include "project/command/PropertyCommands.h"
#include "project/model/Media.h"
#include "project/Editor.h"
#include "render/Flatten.h"
#include "render/MediaCache.h"
#include "render/Resolve.h"
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
namespace render = genesis::render;
namespace ges_adapter = genesis::adapters::engine::ges;
using genesis::core::Rational;

// A private per-run sandbox for the dummy-copy tests, emptied first.
const std::filesystem::path &sandbox()
{
    static const std::filesystem::path dir = [] {
        const std::filesystem::path p =
                std::filesystem::temp_directory_path() / "genesis-reversebuilder-tests";
        std::filesystem::remove_all(p);
        std::filesystem::create_directories(p);
        return p;
    }();
    return dir;
}

bool write_bytes(const std::filesystem::path &path, const std::string &text)
{
    std::ofstream out(path, std::ios::binary | std::ios::trunc);
    out.write(text.data(), static_cast<std::streamsize>(text.size()));
    return static_cast<bool>(out);
}

bool non_empty(const std::filesystem::path &path)
{
    std::error_code ec;
    return std::filesystem::exists(path, ec) && !ec && std::filesystem::file_size(path, ec) > 0
            && !ec;
}

// A one-second VP8/WebM source, generated on demand. An empty path fails the
// generation and e2e tests loudly.
std::string media_fixture()
{
    const std::filesystem::path generated =
            std::filesystem::temp_directory_path() / "genesis-reversebuilder-fixture.webm";
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

// A project with one video media item and one clip, marked reversed.
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

// The empty-root and missing-source guards: nothing to reverse returns
// nullopt and creates nothing.
void test_ensure_reverse_guards()
{
    const std::filesystem::path root = sandbox() / "guards";
    const std::filesystem::path missing = sandbox() / "absent.webm";
    std::filesystem::remove(missing);

    check(!ges_adapter::ensure_reverse(missing, 0.0, 1.0, root).has_value(),
          "a missing source returns nullopt");
    check(!ges_adapter::ensure_reverse(sandbox() / "real.webm", 0.0, 1.0, std::filesystem::path())
                   .has_value(),
          "an empty root returns nullopt");
    check(!ges_adapter::ensure_reverse(std::filesystem::path(), 0.0, 1.0, root).has_value(),
          "an empty source returns nullopt");
}

// A fresh copy is reused: ensure_reverse returns it without regenerating, so
// the pre-written bytes survive untouched.
void test_ensure_reverse_reuses_a_fresh_copy()
{
    const std::filesystem::path root = sandbox() / "reuse";
    std::filesystem::create_directories(root);
    const std::filesystem::path source = sandbox() / "reuse-source.webm";
    write_bytes(source, "source-bytes");
    const std::uintmax_t size = render::cache_source_size(source);
    check(size > 0, "the dummy source has a size");

    const std::filesystem::path destination = render::reverse_cache_path(root, source, 0.0, 1.0);
    check(write_bytes(destination, "already-reversed"), "a reverse file is pre-written");
    check(render::cache_record(destination, render::reverse_key(source, size, 0.0, 1.0)),
          "the pre-written reverse is recorded fresh");

    const std::optional<std::filesystem::path> made =
            ges_adapter::ensure_reverse(source, 0.0, 1.0, root);
    check(made.has_value() && *made == destination, "ensure_reverse returns the fresh copy");
    std::ifstream in(destination, std::ios::binary);
    std::string contents((std::istreambuf_iterator<char>(in)), std::istreambuf_iterator<char>());
    check(contents == "already-reversed", "the fresh copy is reused, not regenerated");

    std::filesystem::remove(destination);
    std::filesystem::remove(destination.string() + ".key");
}

// ensure_reverses skips every clip that does not reverse: a forward clip, a
// still, and an empty-path clip create nothing.
void test_ensure_reverses_skips_non_reversed()
{
    const std::filesystem::path root = sandbox() / "skips";
    std::filesystem::create_directories(root);
    const std::filesystem::path source = sandbox() / "skip-source.webm";
    write_bytes(source, "source-bytes");

    render::ExportClip forward = render::ExportClip::blank(render::ClipKind::Video,
                                                           Rational{ 0, 1 }, Rational{ 1, 1 }, 0);
    forward.path = source.string();
    forward.reverse = false;

    render::ExportClip still = render::ExportClip::blank(render::ClipKind::Image, Rational{ 0, 1 },
                                                         Rational{ 1, 1 }, 0);
    still.path = source.string();
    still.reverse = true;

    render::ExportClip empty = render::ExportClip::blank(render::ClipKind::Video, Rational{ 0, 1 },
                                                         Rational{ 1, 1 }, 0);
    empty.reverse = true;

    const std::vector<render::ExportClip> clips = { forward, still, empty };
    ges_adapter::ensure_reverses(clips, root);

    check(std::filesystem::is_empty(root), "no reverse is generated for non-reversing clips");
}

// A reversed clip's reverse is generated off the real source and reads fresh;
// a re-run finds it fresh and does not re-encode.
void test_ensure_reverse_generates_and_stays_fresh(const std::string &media_path)
{
    const std::filesystem::path root = sandbox() / "generate";
    std::filesystem::create_directories(root);

    const std::optional<std::filesystem::path> made =
            ges_adapter::ensure_reverse(media_path, 0.0, 1.0, root);
    check(made.has_value(), "ensure_reverse generates the copy");
    check(made.has_value() && non_empty(*made), "the generated copy is non-empty");

    const std::filesystem::path destination =
            render::reverse_cache_path(root, media_path, 0.0, 1.0);
    const std::uintmax_t size = render::cache_source_size(media_path);
    check(render::cache_is_fresh(destination, render::reverse_key(media_path, size, 0.0, 1.0)),
          "the generated copy reads fresh");

    const auto first_mtime = std::filesystem::last_write_time(destination);
    const std::optional<std::filesystem::path> again =
            ges_adapter::ensure_reverse(media_path, 0.0, 1.0, root);
    check(again.has_value() && *again == destination, "a re-run returns the same copy");
    check(std::filesystem::last_write_time(destination) == first_mtime,
          "a fresh copy is not re-encoded on re-run");

    // A different span names a different file.
    check(render::reverse_cache_path(root, media_path, 0.5, 0.5) != destination,
          "a different span names a different reverse");

    std::filesystem::remove(destination);
    std::filesystem::remove(destination.string() + ".key");
}

// The end-to-end seam: a reversed clip builds into a GES clip whose URI is the
// reverse file and whose inpoint is zero. The reverse file is a copy of the
// source (recorded fresh), so the check needs no re-encode - it pins the
// substitution the builder applies, not make_reverse.
void test_reversed_clip_builds_ges_from_the_reverse(const std::string &media_path)
{
    const ProjectFixture fixture = project_fixture(media_path);
    (void)fixture.media_id;
    (void)fixture.clip_id;

    const std::filesystem::path root = sandbox() / "e2e";
    std::filesystem::create_directories(root);

    // The reverse file build_timeline will look for: a copy of the source at
    // the reverse path, recorded fresh, standing in for a generated reverse.
    const std::filesystem::path reverse_file =
            render::reverse_cache_path(root, media_path, 0.0, 1.0);
    std::error_code ec;
    std::filesystem::copy_file(media_path, reverse_file, ec);
    check(!ec, "the reverse stand-in is written");
    const std::uintmax_t size = render::cache_source_size(media_path);
    check(render::cache_record(reverse_file, render::reverse_key(media_path, size, 0.0, 1.0)),
          "the reverse stand-in is recorded fresh");

    const std::vector<render::ExportClip> flat =
            render::flatten_timeline(fixture.editor.project(), std::nullopt);
    check(flat.size() == 1 && flat[0].reverse, "one reversed clip flattens");

    render::ExportRequest request;
    request.width = 640;
    request.height = 360;
    const render::BuiltTimeline built = render::build_timeline(
            request, genesis::core::FrameRate::THIRTY, std::span<const render::ExportClip>(flat),
            { }, { }, { }, std::optional<std::filesystem::path>(root));
    check(built.reverse_sources.size() == 1, "the reversed clip carries a reverse substitution");

    GESTimeline *timeline = ges_adapter::build_timeline(built);
    check(timeline != nullptr, "the GES timeline builds");
    if (timeline == nullptr) {
        std::filesystem::remove(reverse_file);
        std::filesystem::remove(reverse_file.string() + ".key");
        return;
    }

    gchar *expected_uri = gst_filename_to_uri(reverse_file.string().c_str(), nullptr);
    check(expected_uri != nullptr, "the expected URI is formed");
    if (expected_uri == nullptr) {
        gst_object_unref(timeline);
        std::filesystem::remove(reverse_file);
        std::filesystem::remove(reverse_file.string() + ".key");
        return;
    }

    GList *layers = ges_timeline_get_layers(timeline);
    GList *clips = layers != nullptr ? ges_layer_get_clips(GES_LAYER(layers->data)) : nullptr;
    check(clips != nullptr && g_list_length(clips) == 1, "one GES clip is built");
    if (clips != nullptr && clips->data != nullptr) {
        const gchar *uri = ges_uri_clip_get_uri(GES_URI_CLIP(clips->data));
        check(uri != nullptr && std::string(uri) == std::string(expected_uri),
              "the GES clip reads the reverse file, not the source");
        const GstClockTime inpoint =
                ges_timeline_element_get_inpoint(GES_TIMELINE_ELEMENT(clips->data));
        check(inpoint == 0, "the reversed clip's inpoint is zero");
    }
    if (clips != nullptr) {
        g_list_free(clips);
    }
    if (layers != nullptr) {
        g_list_free(layers);
    }
    g_free(expected_uri);
    gst_object_unref(timeline);

    std::filesystem::remove(reverse_file);
    std::filesystem::remove(reverse_file.string() + ".key");
}

} // namespace

int main()
{
    gst_init(nullptr, nullptr);
    ges_init();

    test_ensure_reverse_guards();
    test_ensure_reverse_reuses_a_fresh_copy();
    test_ensure_reverses_skips_non_reversed();

    const std::string media = media_fixture();
    check(!media.empty(), "a media fixture is available");
    if (!media.empty()) {
        test_ensure_reverse_generates_and_stays_fresh(media);
        test_reversed_clip_builds_ges_from_the_reverse(media);
        std::filesystem::remove(media);
    }

    std::printf("%d checks, %d failures\n", checks, failures);
    return failures == 0 ? 0 : 1;
}
