// SPDX-License-Identifier: GPL-3.0-or-later
// SPDX-FileCopyrightText: 2026 Genesis-0 contributors

#include <cstddef>
#include <cstdio>
#include <filesystem>
#include <fstream>
#include <string>
#include <system_error>
#include <vector>

#include "extensions/Install.h"
#include "extensions/ExtensionManifest.h"
#include "extensions/Origins.h"
#include "extensions/RemovedList.h"
#include "extensions/Seed.h"
#include "extensions/Trust.h"
#include "render/Catalogue.h"
#include "../support/Checks.h"

namespace {

using genesis::test::check;

bool contains(const std::vector<std::string> &problems, const std::string &needle)
{
    for (const std::string &problem : problems) {
        if (problem.find(needle) != std::string::npos) {
            return true;
        }
    }
    return false;
}

namespace ext = genesis::extensions;

// A scratch area under the system temp dir: `builtin` is the builtin source
// root (the directory seed() reads), `store` the installed-Extension root.
const std::filesystem::path kBase = std::filesystem::temp_directory_path() / "genesis-seed-tests";
const std::filesystem::path kBuiltin = kBase / "builtin";
const std::filesystem::path kStore = kBase / "store";

// A valid native builtin: a manifest plus a placeholder library file
// (install copies the directory; it never loads the library).
void write_native(const std::filesystem::path &dir)
{
    std::filesystem::create_directories(dir);
    std::ofstream out(dir / "extension.toml");
    out << "[extension]\n"
           "id = \"native.one\"\n"
           "name = \"One\"\n"
           "version = 1\n"
           "api = 1\n"
           "entry = \"libfixture.so\"\n";
    std::ofstream lib(dir / "libfixture.so");
    lib << "placeholder";
}

// A valid data-only builtin: a manifest with an [[effect]] block, the unified
// form every extension uses. No explicit `entry`, no library file.
void write_pack(const std::filesystem::path &dir)
{
    std::filesystem::create_directories(dir);
    std::ofstream out(dir / "extension.toml");
    out << "[extension]\n"
           "id = \"pack.two\"\n"
           "name = \"Two\"\n"
           "version = 1\n"
           "api = 1\n"
           "\n"
           "[[effect]]\n"
           "id = \"pack.two\"\n"
           "name = \"Two\"\n"
           "kind = \"effect\"\n"
           "version = 1\n";
}

// A valid data-only native builtin: no `entry`, no library.
void write_data_only(const std::filesystem::path &dir)
{
    std::filesystem::create_directories(dir);
    std::ofstream out(dir / "extension.toml");
    out << "[extension]\n"
           "id = \"data.three\"\n"
           "name = \"Three\"\n"
           "version = 1\n";
}

// Builds the builtin source root once: two valid builtins, one manifest-less
// directory, and a stray file (all four must be skipped-or-seeded correctly).
void build_fixtures()
{
    write_native(kBuiltin / "native.one");
    write_pack(kBuiltin / "pack.two");
    write_data_only(kBuiltin / "data.three");
    std::filesystem::create_directories(kBuiltin / "junk");
    std::ofstream(kBuiltin / "README.md") << "not a builtin";
}

void reset_store()
{
    std::filesystem::remove_all(kStore);
    std::filesystem::create_directories(kStore);
}

void test_seed_installs_builtins()
{
    reset_store();
    const std::vector<std::string> problems = ext::seed(kStore, kBuiltin);
    check(contains(problems, "junk"), "a manifest-less directory is reported");

    const auto packs = ext::installed_packs(kStore);
    check(packs.count("native.one") == 1, "the native builtin installs");
    check(packs.count("pack.two") == 1, "the pack builtin installs");
    check(packs.count("data.three") == 1, "the data-only builtin installs");

    const auto origins = ext::read_origins(kStore);
    check(origins.count("native.one") == 1 && origins.at("native.one") == ext::Origin::Builtin,
          "the native builtin's origin is recorded as Builtin");
    check(origins.count("pack.two") == 1 && origins.at("pack.two") == ext::Origin::Builtin,
          "the pack builtin's origin is recorded as Builtin");
    check(origins.count("data.three") == 1 && origins.at("data.three") == ext::Origin::Builtin,
          "the data-only builtin's origin is recorded as Builtin");
}

void test_seed_skips_tombstoned()
{
    reset_store();
    (void)ext::seed(kStore, kBuiltin);
    check(ext::installed_packs(kStore).count("native.one") == 1,
          "the first seed installs the builtin");

    check(ext::remove(kStore, "native.one"), "tombstoning the id succeeds");
    std::error_code ec;
    std::filesystem::remove_all(kStore / "native.one", ec);

    (void)ext::seed(kStore, kBuiltin);
    check(ext::removed(kStore, "native.one"), "the tombstone survives the re-seed");
    check(ext::installed_packs(kStore).count("native.one") == 0,
          "a tombstoned id is not re-seeded");
}

void test_seed_is_idempotent()
{
    reset_store();
    (void)ext::seed(kStore, kBuiltin);
    (void)ext::seed(kStore, kBuiltin);

    const auto packs = ext::installed_packs(kStore);
    check(packs.count("pack.two") == 1, "the id stays installed");
    if (packs.count("pack.two") == 1) {
        check(packs.at("pack.two").size() == 1, "a second seed adds no duplicate version");
    }
}

void test_seed_absent_root_reports_problem()
{
    reset_store();
    const std::vector<std::string> problems = ext::seed(kStore, kBase / "does-not-exist");
    check(!problems.empty(), "a missing builtin root is reported");
    check(contains(problems, "no builtin extension source"),
          "the problem names the missing source");
}

// Seeds the real builtin root and checks the genesis.packs extension and its
// effect blocks load correctly from the installed extension.toml.
void test_seed_copies_the_official_pack_payload()
{
    reset_store();
    const std::filesystem::path builtin = ext::builtin_root();
    if (builtin.empty()) {
        check(false, "builtin_root() resolves the source extensions/builtin");
        return;
    }
    const std::vector<std::string> problems = ext::seed(kStore, builtin);
    check(problems.empty(), "the builtin root seeds without problems");

    const auto packs = ext::installed_packs(kStore);
    check(packs.count("genesis.packs") == 1, "the data-only genesis.packs builtin installs");
    if (packs.count("genesis.packs") != 1) {
        return;
    }
    const std::filesystem::path ext_dir = packs.at("genesis.packs").begin()->second;
    const std::filesystem::path ext_manifest = ext_dir / "extension.toml";

    const genesis::extensions::NativeLoadResult loaded =
            genesis::extensions::load_native_manifest_file(ext_manifest);
    check(loaded.manifest.has_value(), "the installed manifest loads");
    if (!loaded.manifest) {
        return;
    }
    check(loaded.manifest->effects.size() == 123,
          "the installed pack payload holds 123 packs (saw "
                  + std::to_string(loaded.manifest->effects.size()) + ")");

    const genesis::render::Catalogue catalogue =
            genesis::render::Catalogue::from_manifest(ext_manifest);
    check(catalogue.pack("genesis.invert") != nullptr,
          "the catalogue built over the store root resolves a builtin pack");
}

} // namespace

int main()
{
    std::filesystem::remove_all(kBase); // clear a stale run
    std::filesystem::create_directories(kBase);
    build_fixtures();

    test_seed_installs_builtins();
    test_seed_skips_tombstoned();
    test_seed_is_idempotent();
    test_seed_absent_root_reports_problem();
    test_seed_copies_the_official_pack_payload();

    std::filesystem::remove_all(kBase);

    return genesis::test::summary();
}
