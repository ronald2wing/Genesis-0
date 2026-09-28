// SPDX-License-Identifier: GPL-3.0-or-later
// SPDX-FileCopyrightText: 2026 Genesis-0 contributors

// The processed-media cache sweep's hermetic suite: root enumeration, entry
// size/mtime accounting, oldest-first eviction to a cap, sidecar removal, and
// full clear, all over a temp root. No Qt, no engine, no GL.

#include <chrono>
#include <cstdint>
#include <cstdio>
#include <filesystem>
#include <fstream>
#include <string>
#include <string_view>

#include "../support/Checks.h"
#include "../support/Fs.h"

#include "workspace/CacheSweep.h"

namespace workspace = genesis::workspace;
using genesis::test::check;
using genesis::test::summary;
using genesis::test::write_file;

namespace {

// A scratch directory under the system temp dir, unique per test, removed
// first so a rerun starts clean.
std::filesystem::path scratch(std::string_view name)
{
    const std::filesystem::path dir =
            std::filesystem::temp_directory_path() / ("genesis-cachesweep-" + std::string(name));
    std::error_code error;
    std::filesystem::remove_all(dir, error);
    std::filesystem::create_directories(dir, error);
    return dir;
}

// Writes a data file of `size` bytes and stamps it `age_seconds` in the past
// (so a larger age is an older cache). Returns its path.
void test_roots_layout()
{
    const std::filesystem::path root = scratch("roots") / "cache";
    const std::vector<std::filesystem::path> roots = workspace::cache_roots(root);
    check(roots.size() == 3, "cache_roots returns the flat root plus masks and titles");
    check(roots[0] == root, "the first root is the flat cache root");
    check(roots[1] == root / "masks", "the second root is masks/");
    check(roots[2] == root / "titles", "the third root is titles/");
    check(workspace::cache_roots({ }).empty(), "an empty cache root yields no roots");
}

void test_entries_skip_sidecars_and_tmp()
{
    const std::filesystem::path root = scratch("entries");
    std::filesystem::create_directories(root / "masks");
    write_file(root / "a.peaks", 10, 0);
    write_file(root / "a.peaks.key", 4, 0);
    write_file(root / "b.proxy.webm", 20, 0);
    write_file(root / "b.proxy.webm.key", 4, 0);
    write_file(root / "masks" / "c.mask", 30, 0);
    write_file(root / "masks" / "c.mask.key", 4, 0);
    write_file(root / "masks" / "staging.tmp", 999, 0);

    const std::vector<workspace::CacheEntry> entries =
            workspace::cache_entries(workspace::cache_roots(root));
    check(entries.size() == 3, "entries count only the three data files, not sidecars or tmp");
    check(workspace::cache_size_bytes(entries) == 60, "the summed size is 10 + 20 + 30");
}

void test_sweep_evicts_oldest_first()
{
    const std::filesystem::path root = scratch("sweep");
    std::filesystem::create_directories(root / "masks");
    std::filesystem::create_directories(root / "titles");

    const std::filesystem::path oldest = write_file(root / "oldest.peaks", 10, 30);
    const std::filesystem::path middle = write_file(root / "masks" / "middle.mask", 20, 20);
    const std::filesystem::path newest = write_file(root / "titles" / "newest.png", 30, 10);
    write_file(root / "oldest.peaks.key", 4, 30);
    write_file(root / "masks" / "middle.mask.key", 4, 20);
    write_file(root / "titles" / "newest.png.key", 4, 10);

    const std::vector<std::filesystem::path> roots = workspace::cache_roots(root);
    const workspace::SweepResult result = workspace::sweep(roots, 30);
    check(result.files_evicted == 2, "the two oldest files are evicted");
    check(result.bytes_evicted == 30, "the evicted bytes total 30");
    check(result.bytes_remaining == 30, "30 bytes remain after the sweep");

    check(!std::filesystem::exists(oldest), "the oldest file is gone");
    check(!std::filesystem::exists(middle), "the second-oldest file is gone");
    check(std::filesystem::exists(newest), "the newest file survives");
    check(!std::filesystem::exists(root / "oldest.peaks.key"), "the evicted sidecar is gone");
    check(!std::filesystem::exists(root / "masks" / "middle.mask.key"),
          "the second evicted sidecar is gone");
    check(std::filesystem::exists(root / "titles" / "newest.png.key"),
          "the survivor's sidecar is untouched");
}

void test_sweep_noop_when_under_cap()
{
    const std::filesystem::path root = scratch("noop");
    write_file(root / "a.peaks", 10, 0);
    write_file(root / "b.poster.jpg", 20, 0);

    const workspace::SweepResult result = workspace::sweep(workspace::cache_roots(root), 100);
    check(result.files_evicted == 0, "nothing is evicted under the cap");
    check(result.bytes_remaining == 30, "the full total remains");
    check(std::filesystem::exists(root / "a.peaks"), "no file is removed under the cap");
}

void test_clear_cache_removes_everything()
{
    const std::filesystem::path root = scratch("clear");
    std::filesystem::create_directories(root / "masks");
    write_file(root / "a.peaks", 10, 0);
    write_file(root / "a.peaks.key", 4, 0);
    write_file(root / "masks" / "b.mask", 20, 0);
    write_file(root / "masks" / "b.mask.key", 4, 0);

    const std::size_t removed = workspace::clear_cache(workspace::cache_roots(root));
    check(removed == 2, "clear removes both data files");
    check(!std::filesystem::exists(root / "a.peaks"), "the first data file is gone");
    check(!std::filesystem::exists(root / "a.peaks.key"), "the first sidecar is gone");
    check(!std::filesystem::exists(root / "masks" / "b.mask"), "the subdir data file is gone");
    check(!std::filesystem::exists(root / "masks" / "b.mask.key"), "the subdir sidecar is gone");
    check(workspace::cache_size_bytes(workspace::cache_entries(workspace::cache_roots(root))) == 0,
          "nothing remains after a clear");
}

void test_missing_root_is_empty()
{
    const std::filesystem::path missing = scratch("missing") / "no-such-cache";
    const std::vector<workspace::CacheEntry> entries =
            workspace::cache_entries(workspace::cache_roots(missing));
    check(entries.empty(), "a missing root contributes no entries");
    check(workspace::clear_cache(workspace::cache_roots(missing)) == 0,
          "clearing a missing root removes nothing");
}

} // namespace

int main()
{
    test_roots_layout();
    test_entries_skip_sidecars_and_tmp();
    test_sweep_evicts_oldest_first();
    test_sweep_noop_when_under_cap();
    test_clear_cache_removes_everything();
    test_missing_root_is_empty();

    return summary();
}
