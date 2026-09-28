// SPDX-License-Identifier: GPL-3.0-or-later
// SPDX-FileCopyrightText: 2026 Genesis-0 contributors

#include <cstdint>
#include <cstdio>
#include <filesystem>
#include <fstream>
#include <span>
#include <string>
#include <vector>

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

namespace render = genesis::render;
using genesis::core::Rational;

// A private per-run sandbox, emptied first so a previous crashed run cannot
// leak a fresh-looking cache into the assertions.
const std::filesystem::path &sandbox()
{
    static const std::filesystem::path dir = [] {
        const std::filesystem::path p =
                std::filesystem::temp_directory_path() / "genesis-reversecache-tests";
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

// The source file the reverse key and build_timeline stat: a real, non-empty
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

// The same source and span map to the same file, under the root, with a
// deterministic stem and the reverse extension.
void test_reverse_path_is_stable()
{
    const std::filesystem::path root = sandbox();
    const std::filesystem::path a = render::reverse_cache_path(root, source(), 0.0, 1.0);
    const std::filesystem::path b = render::reverse_cache_path(root, source(), 0.0, 1.0);
    check(a == b, "the same span maps to the same path");
    check(a.parent_path() == root, "the reverse lives under the root");
    check(a.extension() == ".webm", "the reverse is a webm");
    check(a.filename().string().find(".reverse.") != std::string::npos,
          "the name is stamped as a reverse");
}

// Two spans of one source hash apart, and the root does not change the stem.
void test_reverse_path_distinguishes_spans()
{
    const std::filesystem::path root = sandbox();
    const std::filesystem::path whole = render::reverse_cache_path(root, source(), 0.0, 1.0);
    const std::filesystem::path second = render::reverse_cache_path(root, source(), 1.0, 1.0);
    check(whole != second, "two spans hash to different files");

    const std::filesystem::path other_root =
            render::reverse_cache_path(sandbox() / "sub", source(), 0.0, 1.0);
    check(other_root.filename() == whole.filename(), "the stem is root-independent");
    check(other_root.parent_path() == sandbox() / "sub", "the root directs the directory");
}

// The key records the source path, its size, and both span doubles.
void test_reverse_key_fields()
{
    const std::filesystem::path src = sandbox() / "keyed.webm";
    write_bytes(src, "some-bytes");
    const std::uintmax_t size = render::cache_source_size(src);
    check(size > 0, "a real file reports its size");

    const render::CacheKey key = render::reverse_key(src, size, 1.5, 4.0);
    check(key.source == src, "the key records the source");
    check(key.source_size == size, "the key records the size");
    check(key.start == 1.5, "the key records the span start");
    check(key.duration == 4.0, "the key records the span duration");
    check(key.columns == 0 && key.buckets_per_second == 0 && key.at == 0.0,
          "the non-reverse fields stay default");
}

// A missing source reports size 0, so a key made for it is never fresh.
void test_missing_source_sizes_zero()
{
    const std::filesystem::path missing = sandbox() / "absent.webm";
    std::filesystem::remove(missing);
    check(render::cache_source_size(missing) == 0, "a missing source reports size zero");
}

// The full freshness cycle through the recorded key: a written reverse reads
// fresh with the same key and stale when the span moves.
void test_reverse_key_freshness_round_trips()
{
    const std::filesystem::path root = sandbox();
    const std::filesystem::path src = sandbox() / "fresh.webm";
    write_bytes(src, "source-bytes");
    const std::uintmax_t size = render::cache_source_size(src);

    const std::filesystem::path reverse = render::reverse_cache_path(root, src, 0.0, 1.0);
    check(write_bytes(reverse, "reverse-bytes"), "a reverse file is written");

    const render::CacheKey key = render::reverse_key(src, size, 0.0, 1.0);
    check(render::cache_record(reverse, key), "recording succeeds");
    check(render::cache_is_fresh(reverse, key), "the same key reads fresh");

    const render::CacheKey moved = render::reverse_key(src, size, 0.5, 1.0);
    check(!render::cache_is_fresh(reverse, moved), "a different span reads stale");

    std::filesystem::remove(reverse);
    std::filesystem::remove(reverse.string() + ".key");
}

// build_timeline populates `reverse_sources` for a reversed video clip whose
// reverse file is present and fresh, and leaves it empty otherwise: no root,
// a clip not reversed, a non-video kind, or a missing source.
void test_build_timeline_populates_reverse_sources()
{
    const std::filesystem::path root = sandbox() / "timeline-root";
    std::filesystem::create_directories(root);
    const std::filesystem::path src = sandbox() / "timeline.webm";
    write_bytes(src, "source-bytes");
    const std::uintmax_t size = render::cache_source_size(src);
    check(size > 0, "the timeline source exists");

    // The reverse file build_timeline looks for, made fresh so it is found.
    const std::filesystem::path reverse = render::reverse_cache_path(root, src, 0.0, 1.0);
    write_bytes(reverse, "reverse-bytes");
    check(render::cache_record(reverse, render::reverse_key(src, size, 0.0, 1.0)),
          "the reverse is recorded fresh");

    auto reversed_clip = [&src]() {
        render::ExportClip clip = render::ExportClip::blank(render::ClipKind::Video,
                                                            Rational{ 0, 1 }, Rational{ 1, 1 }, 0);
        clip.path = src.string();
        clip.source_start = Rational{ 0, 1 };
        clip.reverse = true;
        return clip;
    };

    const auto build = [&src](const std::vector<render::ExportClip> &clips,
                              std::optional<std::filesystem::path> root) {
        render::ExportRequest request;
        request.width = 320;
        request.height = 240;
        std::vector<render::ExportClip> copy = clips;
        return render::build_timeline(request, genesis::core::FrameRate::THIRTY,
                                      std::span<const render::ExportClip>(copy), { }, { }, { },
                                      root);
    };

    // With the root, a fresh reverse is substituted.
    const std::vector<render::ExportClip> reversed = { reversed_clip() };
    const render::BuiltTimeline with_root =
            build(reversed, std::optional<std::filesystem::path>(root));
    check(with_root.reverse_sources.size() == 1, "a fresh reverse is recorded against its clip");
    for (const auto &[id, path] : with_root.reverse_sources) {
        (void)id;
        check(path == reverse.string(), "the recorded path is the reverse file");
    }

    // Without a root, nothing is substituted (the clip reads forward).
    const render::BuiltTimeline no_root = build(reversed, std::nullopt);
    check(no_root.reverse_sources.empty(), "no root means no reverse substitution");

    // A clip not marked reverse is never substituted, root or not.
    render::ExportClip forward = reversed_clip();
    forward.reverse = false;
    const render::BuiltTimeline not_reversed =
            build({ forward }, std::optional<std::filesystem::path>(root));
    check(not_reversed.reverse_sources.empty(), "a forward clip is not substituted");

    // A still is never substituted: only footage reverses.
    render::ExportClip still = reversed_clip();
    still.kind = render::ClipKind::Image;
    const render::BuiltTimeline image_kind =
            build({ still }, std::optional<std::filesystem::path>(root));
    check(image_kind.reverse_sources.empty(), "a still is not substituted");

    // A source that cannot be statted has no fresh reverse, so no entry.
    render::ExportClip missing = reversed_clip();
    missing.path = (sandbox() / "not-here.webm").string();
    const render::BuiltTimeline missing_source =
            build({ missing }, std::optional<std::filesystem::path>(root));
    check(missing_source.reverse_sources.empty(), "a missing source is not substituted");

    std::filesystem::remove(reverse);
    std::filesystem::remove(reverse.string() + ".key");
}

} // namespace

int main()
{
    test_reverse_path_is_stable();
    test_reverse_path_distinguishes_spans();
    test_reverse_key_fields();
    test_missing_source_sizes_zero();
    test_reverse_key_freshness_round_trips();
    test_build_timeline_populates_reverse_sources();

    std::printf("%d checks, %d failures\n", checks, failures);
    return failures == 0 ? 0 : 1;
}
