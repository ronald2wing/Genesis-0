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

#include "adapters/engine/ges/media/Proxy.h"
#include "adapters/engine/ges/session/GesBuilder.h"
#include "render/MediaCache.h"
#include "render/ProxyCache.h"
#include "render/Resolve.h"
#include "../support/Checks.h"

namespace {

using genesis::test::check;

namespace render = genesis::render;
namespace ges_adapter = genesis::adapters::engine::ges;
using genesis::core::Rational;

// A private per-run sandbox for the dummy-copy tests, emptied first.
const std::filesystem::path &sandbox()
{
    static const std::filesystem::path dir = [] {
        const std::filesystem::path p =
                std::filesystem::temp_directory_path() / "genesis-proxybuilder-tests";
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

// A three-second VP8/WebM source, generated on demand. Three seconds so the
// e2e clip's half-second inpoint plus one-second duration stays inside the
// source. An empty path fails the generation and e2e tests loudly.
std::string media_fixture()
{
    const std::filesystem::path generated =
            std::filesystem::temp_directory_path() / "genesis-proxybuilder-fixture.webm";
    std::filesystem::remove(generated);
    const std::string command = "gst-launch-1.0 -q videotestsrc num-buffers=90 ! videoconvert ! "
                                "vp8enc ! webmmux ! filesink location=\""
            + generated.string() + "\" >/dev/null 2>&1";
    if (std::system(command.c_str()) == 0 && std::filesystem::exists(generated)
        && std::filesystem::file_size(generated) > 0) {
        return generated.string();
    }
    std::filesystem::remove(generated);
    return { };
}

// The empty-root, empty-source and missing-source guards: nothing to proxy
// returns nullopt and creates nothing.
void test_ensure_proxy_guards()
{
    const std::filesystem::path root = sandbox() / "guards";
    const std::filesystem::path missing = sandbox() / "absent.webm";
    std::filesystem::remove(missing);

    check(!ges_adapter::ensure_proxy(missing, root).has_value(),
          "a missing source returns nullopt");
    check(!ges_adapter::ensure_proxy(sandbox() / "real.webm", std::filesystem::path()).has_value(),
          "an empty root returns nullopt");
    check(!ges_adapter::ensure_proxy(std::filesystem::path(), root).has_value(),
          "an empty source returns nullopt");
}

// A fresh proxy is reused: ensure_proxy returns it without regenerating, so
// the pre-written bytes survive untouched.
void test_ensure_proxy_reuses_a_fresh_copy()
{
    const std::filesystem::path root = sandbox() / "reuse";
    std::filesystem::create_directories(root);
    const std::filesystem::path source = sandbox() / "reuse-source.webm";
    write_bytes(source, "source-bytes");
    const std::uintmax_t size = render::cache_source_size(source);
    check(size > 0, "the dummy source has a size");

    const std::filesystem::path destination = render::proxy_cache_path(root, source);
    check(write_bytes(destination, "already-proxied"), "a proxy file is pre-written");
    check(render::cache_record(destination, render::proxy_key(source, size)),
          "the pre-written proxy is recorded fresh");

    const std::optional<std::filesystem::path> made = ges_adapter::ensure_proxy(source, root);
    check(made.has_value() && *made == destination, "ensure_proxy returns the fresh copy");
    std::ifstream in(destination, std::ios::binary);
    std::string contents((std::istreambuf_iterator<char>(in)), std::istreambuf_iterator<char>());
    check(contents == "already-proxied", "the fresh copy is reused, not regenerated");

    std::filesystem::remove(destination);
    std::filesystem::remove(destination.string() + ".key");
}

// A proxy is generated off the real source and reads fresh; a re-run finds it
// fresh and does not re-encode.
void test_ensure_proxy_generates_and_stays_fresh(const std::string &media_path)
{
    const std::filesystem::path root = sandbox() / "generate";
    std::filesystem::create_directories(root);

    const std::optional<std::filesystem::path> made = ges_adapter::ensure_proxy(media_path, root);
    check(made.has_value(), "ensure_proxy generates the proxy");
    check(made.has_value() && non_empty(*made), "the generated proxy is non-empty");

    const std::filesystem::path destination = render::proxy_cache_path(root, media_path);
    const std::uintmax_t size = render::cache_source_size(media_path);
    check(render::cache_is_fresh(destination, render::proxy_key(media_path, size)),
          "the generated proxy reads fresh");

    const auto first_mtime = std::filesystem::last_write_time(destination);
    const std::optional<std::filesystem::path> again = ges_adapter::ensure_proxy(media_path, root);
    check(again.has_value() && *again == destination, "a re-run returns the same proxy");
    check(std::filesystem::last_write_time(destination) == first_mtime,
          "a fresh proxy is not re-encoded on re-run");

    std::filesystem::remove(destination);
    std::filesystem::remove(destination.string() + ".key");
}

// The end-to-end seam: a video clip builds into a GES clip whose URI is the
// proxy file and whose inpoint is the clip's source_start - the proxy is a
// whole-source stand-in, so unlike a reverse the inpoint is kept, not zeroed.
// The proxy file is a copy of the source (recorded fresh), so the check needs
// no re-encode - it pins the substitution the builder applies, not make_proxy.
void test_proxy_clip_builds_ges_from_the_proxy(const std::string &media_path)
{
    const std::filesystem::path root = sandbox() / "e2e";
    std::filesystem::create_directories(root);

    // The proxy file build_timeline will look for: a copy of the source at the
    // proxy path, recorded fresh, standing in for a generated proxy.
    const std::filesystem::path proxy_file = render::proxy_cache_path(root, media_path);
    std::error_code ec;
    std::filesystem::copy_file(media_path, proxy_file, ec);
    check(!ec, "the proxy stand-in is written");
    const std::uintmax_t size = render::cache_source_size(media_path);
    check(render::cache_record(proxy_file, render::proxy_key(media_path, size)),
          "the proxy stand-in is recorded fresh");

    render::ExportClip clip = render::ExportClip::blank(render::ClipKind::Video, Rational{ 0, 1 },
                                                        Rational{ 1, 1 }, 0);
    clip.path = media_path;
    // Half a second in: the proxy keeps this, unlike a reverse's zero.
    clip.source_start = Rational{ 1, 2 };
    const std::vector<render::ExportClip> flat = { clip };

    render::ExportRequest request;
    request.width = 640;
    request.height = 360;
    const render::BuiltTimeline built = render::build_timeline(
            request, genesis::core::FrameRate::THIRTY, std::span<const render::ExportClip>(flat),
            { }, { }, { }, std::nullopt, std::optional<std::filesystem::path>(root));
    check(built.proxy_sources.size() == 1, "the video clip carries a proxy substitution");

    GESTimeline *timeline = ges_adapter::build_timeline(built);
    check(timeline != nullptr, "the GES timeline builds");
    if (timeline == nullptr) {
        std::filesystem::remove(proxy_file);
        std::filesystem::remove(proxy_file.string() + ".key");
        return;
    }

    gchar *expected_uri = gst_filename_to_uri(proxy_file.string().c_str(), nullptr);
    check(expected_uri != nullptr, "the expected URI is formed");
    if (expected_uri == nullptr) {
        gst_object_unref(timeline);
        std::filesystem::remove(proxy_file);
        std::filesystem::remove(proxy_file.string() + ".key");
        return;
    }

    GList *layers = ges_timeline_get_layers(timeline);
    GList *clips = layers != nullptr ? ges_layer_get_clips(GES_LAYER(layers->data)) : nullptr;
    check(clips != nullptr && g_list_length(clips) == 1, "one GES clip is built");
    if (clips != nullptr && clips->data != nullptr) {
        const gchar *uri = ges_uri_clip_get_uri(GES_URI_CLIP(clips->data));
        check(uri != nullptr && std::string(uri) == std::string(expected_uri),
              "the GES clip reads the proxy file, not the source");
        const GstClockTime inpoint =
                ges_timeline_element_get_inpoint(GES_TIMELINE_ELEMENT(clips->data));
        check(inpoint == GST_SECOND / 2, "the proxy clip keeps its source inpoint (half a second)");
    }
    if (clips != nullptr) {
        g_list_free(clips);
    }
    if (layers != nullptr) {
        g_list_free(layers);
    }
    g_free(expected_uri);
    gst_object_unref(timeline);

    std::filesystem::remove(proxy_file);
    std::filesystem::remove(proxy_file.string() + ".key");
}

} // namespace

int main()
{
    gst_init(nullptr, nullptr);
    ges_init();

    test_ensure_proxy_guards();
    test_ensure_proxy_reuses_a_fresh_copy();

    const std::string media = media_fixture();
    check(!media.empty(), "a media fixture is available");
    if (!media.empty()) {
        test_ensure_proxy_generates_and_stays_fresh(media);
        test_proxy_clip_builds_ges_from_the_proxy(media);
        std::filesystem::remove(media);
    }

    return genesis::test::summary();
}
