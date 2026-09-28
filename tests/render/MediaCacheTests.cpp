// SPDX-License-Identifier: GPL-3.0-or-later
// SPDX-FileCopyrightText: 2026 Genesis-0 contributors

#include <cstdint>
#include <cstdio>
#include <filesystem>
#include <fstream>
#include <string>
#include <vector>

#include "../support/Checks.h"
#include "render/MediaCache.h"

namespace {

using genesis::test::check;
namespace render = genesis::render;

// A private per-run sandbox, emptied first so a previous crashed run cannot
// leak a fresh-looking sidecar into the assertions.
const std::filesystem::path &sandbox()
{
    static const std::filesystem::path dir = [] {
        const std::filesystem::path p =
                std::filesystem::temp_directory_path() / "genesis-mediacache-tests";
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

render::CacheKey filmstrip_key()
{
    render::CacheKey key;
    key.source = sandbox() / "clip.webm";
    key.source_size = 1234;
    key.columns = 8;
    key.tile_width = 160;
    key.start = 1.5;
    key.duration = 4.0;
    return key;
}

// The same source maps to the same three files, under the root, with a stable
// hash stem and no shaping parameters in the name.
void test_cache_paths_are_stable_and_named_by_kind()
{
    const std::filesystem::path root = sandbox();
    const std::filesystem::path source = "/srv/media/clip.webm";

    const render::CachePaths a = render::cache_paths(root, source);
    const render::CachePaths b = render::cache_paths(root, source);

    check(a.waveform == b.waveform && a.filmstrip == b.filmstrip && a.poster == b.poster,
          "the same source maps to the same paths");
    check(a.waveform.parent_path() == root, "the waveform lives under the root");
    check(a.waveform.extension() == ".peaks", "the waveform is a peaks file");
    check(a.filmstrip.extension() == ".jpg", "the filmstrip is a jpeg");
    check(a.poster.extension() == ".jpg", "the poster is a jpeg");
    check(a.waveform != a.filmstrip && a.filmstrip != a.poster && a.poster != a.waveform,
          "the three kinds are distinct files");
    check(a.waveform.filename() != "clip.webm" && a.waveform.filename() != "clip.peaks",
          "the stem is a hash, not the basename");
}

// Two sources never collide, and the root does not change the stem.
void test_cache_paths_distinguish_sources()
{
    const std::filesystem::path root = sandbox();
    const std::filesystem::path one = "/srv/media/a.webm";
    const std::filesystem::path two = "/srv/media/b.webm";

    const render::CachePaths a = render::cache_paths(root, one);
    const render::CachePaths b = render::cache_paths(root, two);
    check(a.waveform != b.waveform, "different sources hash to different files");

    const render::CachePaths other_root = render::cache_paths(sandbox() / "sub", one);
    check(other_root.waveform.filename() == a.waveform.filename(),
          "the stem is the root-independent hash");
    check(other_root.waveform.parent_path() == sandbox() / "sub", "the root directs the directory");
}

// Nothing on disk: never fresh.
void test_missing_cache_is_not_fresh()
{
    const render::CachePaths paths = render::cache_paths(sandbox(), sandbox() / "absent.webm");
    check(!render::cache_is_fresh(paths.waveform, render::CacheKey{ }),
          "a missing cache file is not fresh");
}

// Recording requires the data file to already exist: the sidecar records its
// size, which cannot be captured from a file that is not there.
void test_cannot_record_without_a_cache_file()
{
    const std::filesystem::path cache = sandbox() / "no-such.peaks";
    check(!render::cache_record(cache, filmstrip_key()), "recording without a cache file fails");
}

// The full cycle: write a data file, record its key, and it reads fresh with
// the same key.
void test_record_then_fresh_round_trips()
{
    const render::CachePaths paths = render::cache_paths(sandbox(), sandbox() / "clip.webm");
    check(write_bytes(paths.filmstrip, "jpeg-bytes-here"), "a data file is written");

    const render::CacheKey key = filmstrip_key();
    check(render::cache_record(paths.filmstrip, key), "recording succeeds");
    check(render::cache_is_fresh(paths.filmstrip, key), "the same key reads fresh");
    // Every recorded field round-trips through the sidecar, so a key that
    // differs in any field reads stale - sampled across the fields below.
    std::filesystem::remove(paths.filmstrip);
    std::filesystem::remove(paths.filmstrip.string() + ".key");
}

// A key that differs from what was recorded reads stale - per field.
void test_a_changed_key_reads_stale()
{
    const render::CachePaths paths = render::cache_paths(sandbox(), sandbox() / "clip.webm");
    check(write_bytes(paths.waveform, "peaks-bytes"), "a data file is written");

    const render::CacheKey key = filmstrip_key();
    check(render::cache_record(paths.waveform, key), "recording succeeds");
    check(render::cache_is_fresh(paths.waveform, key), "baseline is fresh");

    auto changed = key;
    changed.source_size += 1;
    check(!render::cache_is_fresh(paths.waveform, changed), "a changed source size reads stale");
    changed = key;
    changed.columns += 1;
    check(!render::cache_is_fresh(paths.waveform, changed), "a changed column count reads stale");
    changed = key;
    changed.tile_width += 1;
    check(!render::cache_is_fresh(paths.waveform, changed), "a changed tile width reads stale");
    changed = key;
    changed.start += 0.25;
    check(!render::cache_is_fresh(paths.waveform, changed), "a changed start reads stale");
    changed = key;
    changed.duration = 9.0;
    check(!render::cache_is_fresh(paths.waveform, changed), "a changed duration reads stale");
    changed = key;
    changed.buckets_per_second = 100;
    check(!render::cache_is_fresh(paths.waveform, changed), "a changed bucket rate reads stale");
    changed = key;
    changed.source = sandbox() / "other.webm";
    check(!render::cache_is_fresh(paths.waveform, changed), "a changed source path reads stale");

    std::filesystem::remove(paths.waveform);
    std::filesystem::remove(paths.waveform.string() + ".key");
}

// A data file that shrank after recording reads stale: the recorded size no
// longer matches, so a truncation is never mistaken for a good cache.
void test_a_truncated_cache_reads_stale()
{
    const render::CachePaths paths = render::cache_paths(sandbox(), sandbox() / "clip.webm");
    check(write_bytes(paths.poster, "0123456789"), "a data file is written");

    const render::CacheKey key = filmstrip_key();
    check(render::cache_record(paths.poster, key), "recording succeeds");
    check(render::cache_is_fresh(paths.poster, key), "baseline is fresh");

    check(write_bytes(paths.poster, "012"), "the data file is truncated");
    check(!render::cache_is_fresh(paths.poster, key), "a truncated cache reads stale");

    std::filesystem::remove(paths.poster);
    std::filesystem::remove(paths.poster.string() + ".key");
}

// A sidecar that cannot decode (wrong magic, short) reads stale, never crashes.
void test_a_corrupt_sidecar_reads_stale()
{
    const render::CachePaths paths = render::cache_paths(sandbox(), sandbox() / "clip.webm");
    check(write_bytes(paths.waveform, "peaks-bytes"), "a data file is written");

    const render::CacheKey key = filmstrip_key();
    check(render::cache_record(paths.waveform, key), "recording succeeds");

    const std::filesystem::path sidecar = paths.waveform.string() + ".key";
    check(write_bytes(sidecar, "garbage-not-a-key"), "the sidecar is overwritten with garbage");
    check(!render::cache_is_fresh(paths.waveform, key), "a corrupt sidecar reads stale");
    check(write_bytes(sidecar, "GK"), "the sidecar is truncated");
    check(!render::cache_is_fresh(paths.waveform, key), "a truncated sidecar reads stale");

    std::filesystem::remove(paths.waveform);
    std::filesystem::remove(sidecar);
}

} // namespace

int main()
{
    test_cache_paths_are_stable_and_named_by_kind();
    test_cache_paths_distinguish_sources();
    test_missing_cache_is_not_fresh();
    test_cannot_record_without_a_cache_file();
    test_record_then_fresh_round_trips();
    test_a_changed_key_reads_stale();
    test_a_truncated_cache_reads_stale();
    test_a_corrupt_sidecar_reads_stale();

    return genesis::test::summary();
}
