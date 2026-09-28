// SPDX-License-Identifier: GPL-3.0-or-later
// SPDX-FileCopyrightText: 2026 Genesis-0 contributors

#include <cstdio>
#include <filesystem>
#include <string>
#include <string_view>

#include "workspace/AssetPaths.h"
#include "../support/Checks.h"

namespace workspace = genesis::workspace;

namespace {

using genesis::test::check;

// A scratch directory under the system temp dir, unique per test, removed
// first so a rerun starts clean.
std::filesystem::path scratch(std::string_view name)
{
    const std::filesystem::path dir =
            std::filesystem::temp_directory_path() / ("genesis-assetpaths-" + std::string(name));
    std::error_code error;
    std::filesystem::remove_all(dir, error);
    std::filesystem::create_directories(dir, error);
    return dir;
}

void test_installed_share_root_layout()
{
    check(workspace::installed_share_root("/opt/app/bin") == "/opt/app/share/genesis-0",
          "the share root is <bindir>/../share/genesis-0");
    check(workspace::installed_share_root({ }).empty(),
          "an empty executable dir has no share root");
}

void test_env_override_wins()
{
    const std::filesystem::path env = scratch("env") / "assets";
    const std::filesystem::path compiled = scratch("compiled") / "assets";
    std::filesystem::create_directories(env);
    std::filesystem::create_directories(compiled);
    const std::filesystem::path found =
            workspace::resolve_asset_root("/opt/app/bin", "/tmp", env.c_str(), compiled.c_str());
    check(found == env, "the environment override beats the compile-time dir");
}

void test_compile_time_dir_used_when_env_absent()
{
    const std::filesystem::path compiled = scratch("compiled-only") / "assets";
    std::filesystem::create_directories(compiled);
    const std::filesystem::path found =
            workspace::resolve_asset_root("/opt/app/bin", "/tmp", nullptr, compiled.c_str());
    check(found == compiled, "the compile-time dir resolves when the env var is unset");
}

void test_a_missing_env_dir_falls_through()
{
    const std::filesystem::path missing = scratch("missing-env") / "no-such-dir";
    const std::filesystem::path compiled = scratch("fallback") / "assets";
    std::filesystem::create_directories(compiled);
    const std::filesystem::path found = workspace::resolve_asset_root(
            "/opt/app/bin", "/tmp", missing.c_str(), compiled.c_str());
    check(found == compiled, "a non-existent env dir does not shadow the compile-time dir");
}

void test_walk_up_from_cwd()
{
    const std::filesystem::path root = scratch("walkup");
    const std::filesystem::path assets = root / "assets";
    const std::filesystem::path cwd = root / "sub" / "deeper";
    std::filesystem::create_directories(assets);
    std::filesystem::create_directories(cwd);
    const std::filesystem::path found =
            workspace::resolve_asset_root(root / "bin", cwd, nullptr, nullptr);
    check(found == assets, "the walk-up from cwd finds an ancestor assets/ dir");
}

void test_installed_prefix()
{
    const std::filesystem::path root = scratch("installed");
    const std::filesystem::path exe = root / "bin";
    const std::filesystem::path assets = root / "share" / "genesis-0" / "assets";
    const std::filesystem::path cwd = root / "cwd";
    std::filesystem::create_directories(exe);
    std::filesystem::create_directories(assets);
    std::filesystem::create_directories(cwd);
    const std::filesystem::path found = workspace::resolve_asset_root(exe, cwd, nullptr, nullptr);
    check(found == assets, "the packaged prefix <bindir>/../share/genesis-0/assets resolves");
}

void test_walk_up_from_exe_dir()
{
    const std::filesystem::path root = scratch("exedir");
    const std::filesystem::path exe = root / "bin";
    const std::filesystem::path assets = root / "assets";
    const std::filesystem::path cwd = root / "cwd";
    std::filesystem::create_directories(exe);
    std::filesystem::create_directories(assets);
    std::filesystem::create_directories(cwd);
    const std::filesystem::path found = workspace::resolve_asset_root(exe, cwd, nullptr, nullptr);
    check(found == assets, "the final fallback walks up from the executable's directory");
}

void test_nothing_resolves()
{
    const std::filesystem::path root = scratch("nothing");
    const std::filesystem::path exe = root / "bin";
    const std::filesystem::path cwd = root / "cwd";
    std::filesystem::create_directories(exe);
    std::filesystem::create_directories(cwd);
    const std::filesystem::path found = workspace::resolve_asset_root(exe, cwd, nullptr, nullptr);
    check(found.empty(), "an empty path is returned when nothing resolves");
}

} // namespace

int main()
{
    test_installed_share_root_layout();
    test_env_override_wins();
    test_compile_time_dir_used_when_env_absent();
    test_a_missing_env_dir_falls_through();
    test_walk_up_from_cwd();
    test_installed_prefix();
    test_walk_up_from_exe_dir();
    test_nothing_resolves();

    return genesis::test::summary();
}
