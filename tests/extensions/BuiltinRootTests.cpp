// SPDX-License-Identifier: GPL-3.0-or-later
// SPDX-FileCopyrightText: 2026 Genesis-0 contributors

#include <cstdio>
#include <filesystem>
#include <string>
#include <string_view>

#include "extensions/Seed.h"
#include "../support/Checks.h"

namespace extensions = genesis::extensions;

namespace {

using genesis::test::check;

// A scratch directory under the system temp dir, unique per test, removed
// first so a rerun starts clean.
std::filesystem::path scratch(std::string_view name)
{
    const std::filesystem::path dir =
            std::filesystem::temp_directory_path() / ("genesis-builtinroot-" + std::string(name));
    std::error_code error;
    std::filesystem::remove_all(dir, error);
    std::filesystem::create_directories(dir, error);
    return dir;
}

void test_compile_time_dir_wins()
{
    const std::filesystem::path compiled = scratch("compiled") / "builtin";
    const std::filesystem::path exe = scratch("exe") / "bin";
    const std::filesystem::path beside = exe / "extensions" / "builtin";
    std::filesystem::create_directories(compiled);
    std::filesystem::create_directories(beside);
    const std::filesystem::path found = extensions::resolve_builtin_root(exe, compiled.c_str());
    check(found == compiled, "the compile-time dir beats the beside-the-exe copy");
}

void test_beside_the_exe()
{
    const std::filesystem::path exe = scratch("beside") / "bin";
    const std::filesystem::path beside = exe / "extensions" / "builtin";
    std::filesystem::create_directories(beside);
    const std::filesystem::path found = extensions::resolve_builtin_root(exe, nullptr);
    check(found == beside, "the beside-the-exe copy resolves when there is no compile-time dir");
}

void test_installed_prefix()
{
    const std::filesystem::path root = scratch("installed");
    const std::filesystem::path exe = root / "bin";
    const std::filesystem::path installed = root / "share" / "genesis-0" / "extensions" / "builtin";
    std::filesystem::create_directories(exe);
    std::filesystem::create_directories(installed);
    const std::filesystem::path found = extensions::resolve_builtin_root(exe, nullptr);
    check(found == installed,
          "the packaged prefix <bindir>/../share/genesis-0/extensions/builtin resolves");
}

void test_compile_time_beats_installed()
{
    const std::filesystem::path compiled = scratch("ct-first") / "builtin";
    const std::filesystem::path root = scratch("ct-first-root");
    const std::filesystem::path exe = root / "bin";
    const std::filesystem::path installed = root / "share" / "genesis-0" / "extensions" / "builtin";
    std::filesystem::create_directories(compiled);
    std::filesystem::create_directories(installed);
    const std::filesystem::path found = extensions::resolve_builtin_root(exe, compiled.c_str());
    check(found == compiled, "the compile-time dir beats the installed prefix");
}

void test_nothing_resolves()
{
    const std::filesystem::path exe = scratch("nothing") / "bin";
    std::filesystem::create_directories(exe);
    const std::filesystem::path found = extensions::resolve_builtin_root(exe, nullptr);
    check(found.empty(), "an empty path is returned when nothing resolves");
}

} // namespace

int main()
{
    test_compile_time_dir_wins();
    test_beside_the_exe();
    test_installed_prefix();
    test_compile_time_beats_installed();
    test_nothing_resolves();

    return genesis::test::summary();
}
