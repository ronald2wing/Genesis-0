// SPDX-License-Identifier: GPL-3.0-or-later
// SPDX-FileCopyrightText: 2026 Genesis-0 contributors

// The cache controller's suite: drives size/sweep/clear off the UI thread over
// a temp cache root, proving the work leaves the controller's thread
// (workerThreadId) and the size settles after each run. No GStreamer, no GL.

#include <chrono>
#include <cstdio>
#include <filesystem>
#include <fstream>
#include <string>

#include <QCoreApplication>
#include <QThread>

#include "../support/Checks.h"
#include "../support/Fs.h"
#include "../support/Wait.h"

#include "app/controllers/CacheController.h"

#include "workspace/CacheSweep.h"

namespace {

using genesis::test::check;
using genesis::test::summary;
using genesis::test::wait_until;
using genesis::test::write_file;

std::filesystem::path scratch()
{
    const std::filesystem::path dir =
            std::filesystem::temp_directory_path() / "genesis-cachecontroller-test";
    std::error_code error;
    std::filesystem::remove_all(dir, error);
    std::filesystem::create_directories(dir, error);
    return dir;
}

void test_defaults()
{
    CacheController controller;
    check(controller.sizeBytes() == 0, "size starts at zero");
    check(!controller.running(), "not running before any operation");
    check(controller.capBytes() == genesis::workspace::kDefaultCacheCapBytes,
          "the cap defaults to 10 GiB");
}

void test_refresh_size_populates()
{
    const std::filesystem::path root = scratch();
    std::filesystem::create_directories(root / "masks");
    write_file(root / "a.peaks", 10, 0);
    write_file(root / "a.peaks.key", 4, 0);
    write_file(root / "masks" / "b.mask", 20, 0);

    CacheController controller;
    controller.setCacheRoot(root);
    controller.refreshSize();

    check(wait_until([&] { return !controller.running(); }), "the refresh run settles");
    check(controller.sizeBytes() == 30, "the size sums the two data files, not the sidecar");
    check(controller.workerThreadId() != reinterpret_cast<quintptr>(QThread::currentThread()),
          "the scan left the controller's thread");
}

void test_sweep_evicts_to_cap()
{
    const std::filesystem::path root = scratch();
    std::filesystem::create_directories(root / "masks");
    write_file(root / "oldest.peaks", 40, 30);
    write_file(root / "masks" / "newest.mask", 20, 10);

    CacheController controller;
    controller.setCacheRoot(root);
    controller.setCapBytes(25);
    controller.sweep();

    check(wait_until([&] { return !controller.running(); }), "the sweep run settles");
    check(controller.sizeBytes() == 20, "the oldest file is evicted, the newest remains");
    check(!std::filesystem::exists(root / "oldest.peaks"), "the evicted file is gone");
    check(std::filesystem::exists(root / "masks" / "newest.mask"), "the newest file survives");
}

void test_clear_empties()
{
    const std::filesystem::path root = scratch();
    std::filesystem::create_directories(root / "masks");
    write_file(root / "a.peaks", 10, 0);
    write_file(root / "masks" / "b.mask", 20, 0);

    CacheController controller;
    controller.setCacheRoot(root);
    controller.refreshSize();
    check(wait_until([&] { return !controller.running(); }), "the size run settles");
    check(controller.sizeBytes() == 30, "the size is reported before clearing");

    controller.clearCache();
    check(wait_until([&] { return !controller.running(); }), "the clear run settles");
    check(controller.sizeBytes() == 0, "the size reports zero after clearing");
    check(!std::filesystem::exists(root / "a.peaks"), "the first file is gone");
    check(!std::filesystem::exists(root / "masks" / "b.mask"), "the second file is gone");
}

void test_empty_root_is_a_noop()
{
    CacheController controller;
    controller.refreshSize();
    check(!controller.running(), "no run starts with no root set");
    check(controller.sizeBytes() == 0, "the size stays zero");
}

} // namespace

int main(int argc, char **argv)
{
    QCoreApplication app(argc, argv);

    test_defaults();
    test_refresh_size_populates();
    test_sweep_evicts_to_cap();
    test_clear_empties();
    test_empty_root_is_a_noop();

    return summary();
}
