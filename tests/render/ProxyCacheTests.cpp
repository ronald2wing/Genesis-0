// SPDX-License-Identifier: GPL-3.0-or-later
// SPDX-FileCopyrightText: 2026 Genesis-0 contributors

#include <cstdint>
#include <cstdio>
#include <filesystem>
#include <fstream>
#include <span>
#include <string>
#include <vector>

#include "../support/Checks.h"
#include "render/ProxyCache.h"
#include "render/Resolve.h"

namespace {

using genesis::test::check;
namespace render = genesis::render;
using genesis::core::Rational;

// A private per-run sandbox, emptied first so a previous crashed run cannot
// leak a fresh-looking cache into the assertions.
const std::filesystem::path &sandbox()
{
    static const std::filesystem::path dir = [] {
        const std::filesystem::path p =
                std::filesystem::temp_directory_path() / "genesis-proxycache-tests";
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

// The source file the proxy key and build_timeline stat: a real, non-empty
// file so its size is stable and non-zero.
const std::filesystem::path source()
{
    static const std::filesystem::path p = [] {
        const std::filesystem::path f = sandbox() / "source.webm";
        write_bytes(f, "source-bytes");
        return f;
    }();
    return p;
}

// The same source maps to the same file, under the root, with a deterministic
// stem and the proxy extension.
void test_proxy_path_is_stable()
{
    const std::filesystem::path root = sandbox();
    const std::filesystem::path a = render::proxy_cache_path(root, source());
    const std::filesystem::path b = render::proxy_cache_path(root, source());
    check(a == b, "the same source maps to the same path");
    check(a.parent_path() == root, "the proxy lives under the root");
    check(a.extension() == ".webm", "the proxy is a webm");
    check(a.filename().string().find(".proxy.") != std::string::npos,
          "the name is stamped as a proxy");
}

// Two sources hash apart, and the root does not change the stem. The proxy's
// stem matches the poster's (same FNV-1a hash, different extension), so a
// proxy lands beside its sibling caches.
void test_proxy_path_distinguishes_sources()
{
    const std::filesystem::path root = sandbox();
    const std::filesystem::path other = sandbox() / "other.webm";
    write_bytes(other, "other-bytes");

    const std::filesystem::path a = render::proxy_cache_path(root, source());
    const std::filesystem::path b = render::proxy_cache_path(root, other);
    check(a != b, "two sources hash to different files");

    const std::filesystem::path moved = render::proxy_cache_path(sandbox() / "sub", source());
    check(moved.filename() == a.filename(), "the stem is root-independent");
    check(moved.parent_path() == sandbox() / "sub", "the root directs the directory");

    const render::CachePaths paths = render::cache_paths(root, source());
    check(a.filename().stem().string().substr(0, 16)
                  == paths.poster.filename().stem().string().substr(0, 16),
          "the proxy shares the poster's hash stem");
}

// The key records the source path and its size, and nothing else.
void test_proxy_key_fields()
{
    const std::filesystem::path src = sandbox() / "keyed.webm";
    write_bytes(src, "some-bytes");
    const std::uintmax_t size = render::cache_source_size(src);
    check(size > 0, "a real file reports its size");

    const render::CacheKey key = render::proxy_key(src, size);
    check(key.source == src, "the key records the source");
    check(key.source_size == size, "the key records the size");
    check(key.start == 0.0 && key.duration == 0.0 && key.at == 0.0 && key.columns == 0
                  && key.buckets_per_second == 0 && key.tile_width == 0,
          "the non-proxy fields stay default");
}

// The full freshness cycle through the recorded key: a written proxy reads
// fresh with the same key and stale when the source's size changes.
void test_proxy_key_freshness_round_trips()
{
    const std::filesystem::path root = sandbox();
    const std::filesystem::path src = sandbox() / "fresh.webm";
    write_bytes(src, "source-bytes");
    const std::uintmax_t size = render::cache_source_size(src);

    const std::filesystem::path proxy = render::proxy_cache_path(root, src);
    check(write_bytes(proxy, "proxy-bytes"), "a proxy file is written");

    const render::CacheKey key = render::proxy_key(src, size);
    check(render::cache_record(proxy, key), "recording succeeds");
    check(render::cache_is_fresh(proxy, key), "the same key reads fresh");

    write_bytes(src, "source-bytes-that-grew");
    const render::CacheKey grown = render::proxy_key(src, render::cache_source_size(src));
    check(!render::cache_is_fresh(proxy, grown), "a changed source size reads stale");

    std::filesystem::remove(proxy);
    std::filesystem::remove(proxy.string() + ".key");
}

// build_timeline populates `proxy_sources` for a video clip whose proxy file
// is present and fresh, and leaves it empty otherwise: no root, a non-video
// kind, a missing source, or a reversed clip (the reverse owns that playback).
void test_build_timeline_populates_proxy_sources()
{
    const std::filesystem::path root = sandbox() / "timeline-root";
    std::filesystem::create_directories(root);
    const std::filesystem::path src = sandbox() / "timeline.webm";
    write_bytes(src, "source-bytes");
    const std::uintmax_t size = render::cache_source_size(src);
    check(size > 0, "the timeline source exists");

    // The proxy file build_timeline looks for, made fresh so it is found.
    const std::filesystem::path proxy = render::proxy_cache_path(root, src);
    write_bytes(proxy, "proxy-bytes");
    check(render::cache_record(proxy, render::proxy_key(src, size)), "the proxy is recorded fresh");

    auto video_clip = [&src]() {
        render::ExportClip clip = render::ExportClip::blank(render::ClipKind::Video,
                                                            Rational{ 0, 1 }, Rational{ 1, 1 }, 0);
        clip.path = src.string();
        clip.source_start = Rational{ 0, 1 };
        return clip;
    };

    const auto build = [](const std::vector<render::ExportClip> &clips,
                          std::optional<std::filesystem::path> root) {
        render::ExportRequest request;
        request.width = 320;
        request.height = 240;
        std::vector<render::ExportClip> copy = clips;
        return render::build_timeline(request, genesis::core::FrameRate::THIRTY,
                                      std::span<const render::ExportClip>(copy), { }, { }, { },
                                      std::nullopt, root);
    };

    // With the root, a fresh proxy is substituted.
    const std::vector<render::ExportClip> video = { video_clip() };
    const render::BuiltTimeline with_root =
            build(video, std::optional<std::filesystem::path>(root));
    check(with_root.proxy_sources.size() == 1, "a fresh proxy is recorded against its clip");
    for (const auto &[id, path] : with_root.proxy_sources) {
        (void)id;
        check(path == proxy.string(), "the recorded path is the proxy file");
    }

    // Without a root, nothing is substituted (the clip reads the source).
    const render::BuiltTimeline no_root = build(video, std::nullopt);
    check(no_root.proxy_sources.empty(), "no root means no proxy substitution");

    // A still is never substituted: only footage proxies.
    render::ExportClip still = video_clip();
    still.kind = render::ClipKind::Image;
    const render::BuiltTimeline image_kind =
            build({ still }, std::optional<std::filesystem::path>(root));
    check(image_kind.proxy_sources.empty(), "a still is not substituted");

    // A source that cannot be statted has no fresh proxy, so no entry.
    render::ExportClip missing = video_clip();
    missing.path = (sandbox() / "not-here.webm").string();
    const render::BuiltTimeline missing_source =
            build({ missing }, std::optional<std::filesystem::path>(root));
    check(missing_source.proxy_sources.empty(), "a missing source is not substituted");

    // A reversed clip reads its reverse, never the proxy.
    render::ExportClip reversed = video_clip();
    reversed.reverse = true;
    const render::BuiltTimeline reversed_clip =
            build({ reversed }, std::optional<std::filesystem::path>(root));
    check(reversed_clip.proxy_sources.empty(), "a reversed clip is not substituted");

    std::filesystem::remove(proxy);
    std::filesystem::remove(proxy.string() + ".key");
}

} // namespace

int main()
{
    test_proxy_path_is_stable();
    test_proxy_path_distinguishes_sources();
    test_proxy_key_fields();
    test_proxy_key_freshness_round_trips();
    test_build_timeline_populates_proxy_sources();

    return genesis::test::summary();
}
