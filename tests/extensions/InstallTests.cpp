// SPDX-License-Identifier: GPL-3.0-or-later
// SPDX-FileCopyrightText: 2026 Genesis-0 contributors

#include <cstdio>
#include <filesystem>
#include <fstream>
#include <map>
#include <string>
#include <vector>

#include "extensions/Install.h"
#include "extensions/Origins.h"
#include "extensions/Sources.h"

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

// A scratch area under the system temp dir, cleared at the start of the run and
// removed at the end. `store` is the install root; each Pack fixture is written
// beside it.
const std::filesystem::path kBase =
        std::filesystem::temp_directory_path() / "genesis-install-tests";
const std::filesystem::path kStore = kBase / "store";

// Writes a Pack directory (an effect.toml with the given body) and returns it.
std::filesystem::path write_pack(const std::string &name, const std::string &manifest_text)
{
    const std::filesystem::path dir = kBase / name;
    std::filesystem::create_directories(dir);
    std::ofstream out(dir / "effect.toml");
    out << manifest_text;
    return dir;
}

// A minimal, valid format-2 manifest for the given id and version.
std::string manifest(const std::string &id, int version)
{
    return "format = 2\n\n"
           "[effect]\n"
           "id = \""
            + id
            + "\"\n"
              "name = \"Blur\"\n"
              "kind = \"effect\"\n"
              "version = "
            + std::to_string(version) + "\n";
}

// A manifest that does not parse: a broken `[effect` table header.
constexpr const char *kMalformed = "format = 2\n[effect\nid = \"test.bad\"\n";

// Writes a native-extension directory (an extension.toml plus a placeholder
// library file) and returns it. Install never loads the library, so the
// placeholder only has to exist for the copy to carry it.
std::filesystem::path write_native(const std::string &name, const std::string &manifest_text)
{
    const std::filesystem::path dir = kBase / name;
    std::filesystem::create_directories(dir);
    {
        std::ofstream out(dir / "extension.toml");
        out << manifest_text;
    }
    {
        std::ofstream out(dir / "libfixture.so");
        out << "placeholder";
    }
    return dir;
}

// A minimal, valid format-1 native manifest for the given id.
std::string native_manifest(const std::string &id)
{
    return "format = 1\n\n"
           "[extension]\n"
           "id = \""
            + id
            + "\"\n"
              "name = \"Fixture\"\n"
              "kind = \"native\"\n"
              "version = 1\n"
              "api = 1\n"
              "entry = \"libfixture.so\"\n";
}

// A minimal, valid format-2 data-only native manifest: no `entry`, no library.
std::string data_only_native_manifest(const std::string &id)
{
    return "format = 2\n\n"
           "[extension]\n"
           "id = \""
            + id
            + "\"\n"
              "name = \"Data Only\"\n"
              "kind = \"native\"\n"
              "version = 1\n"
              "api = 1\n";
}

void test_trust_model()
{
    const genesis::effects::Pack pack; // permissions_for does not read the
    // schema today; see Trust.cpp.
    check(ext::is_trusted(ext::Origin::Builtin), "Builtin is trusted");
    check(ext::is_trusted(ext::Origin::Curated), "Curated is trusted");
    check(!ext::is_trusted(ext::Origin::Developer), "Developer is not trusted");
    check(!ext::is_trusted(ext::Origin::User), "User is not trusted");

    check(ext::may_install_native(ext::Origin::Builtin), "Builtin may install a native extension");
    check(ext::may_install_native(ext::Origin::Curated), "Curated may install a native extension");
    check(ext::may_install_native(ext::Origin::Developer),
          "Developer may install (visible, load-gated) a native extension");
    check(!ext::may_install_native(ext::Origin::User), "User may not install a native extension");

    check(ext::may_load_native(ext::Origin::Builtin, false), "Builtin loads without consent");
    check(ext::may_load_native(ext::Origin::Developer, true), "Developer loads only with consent");
    check(!ext::may_load_native(ext::Origin::Developer, false),
          "Developer is refused without consent");
    check(!ext::may_load_native(ext::Origin::User, true), "User is refused even with consent");

    const ext::Permission builtin = ext::permissions_for(ext::Origin::Builtin, pack);
    check(builtin.execute, "Builtin may contribute a shader");
    check(!builtin.filesystem && !builtin.network, "a Pack never gets filesystem or network");

    const ext::Permission curated = ext::permissions_for(ext::Origin::Curated, pack);
    check(curated.execute, "Curated may contribute a shader");
    check(!curated.filesystem && !curated.network, "still no filesystem or network");

    const ext::Permission user = ext::permissions_for(ext::Origin::User, pack);
    check(!user.execute, "User may not contribute a shader");
}

void test_valid_pack_installs()
{
    const auto source = write_pack("blur-v1", manifest("test.blur", 1));
    const ext::InstallResult result = ext::install_extension(source, kStore, ext::Origin::Builtin);
    check(result.ok(), "a valid Pack installs");
    check(result.installed.has_value(), "and returns its root");
    if (result.installed) {
        check(std::filesystem::exists(*result.installed), "the installed root exists");
        check(std::filesystem::exists(*result.installed / "effect.toml"), "and holds the manifest");
    }
    const auto packs = ext::installed_packs(kStore);
    check(packs.count("test.blur") == 1, "the id appears in installed_packs");
    if (packs.count("test.blur") == 1) {
        check(packs.at("test.blur").count("1") == 1, "with version 1 listed");
    }

    // Curated (the signed marketplace) is trusted too, so it also installs.
    const auto curated = write_pack("curated", manifest("test.curated", 1));
    check(ext::install_extension(curated, kStore, ext::Origin::Curated).ok(),
          "a Curated Pack installs");
}

void test_malformed_manifest_refused()
{
    const auto source = write_pack("bad", kMalformed);
    const ext::InstallResult result = ext::install_extension(source, kStore, ext::Origin::Builtin);
    check(!result.ok(), "a malformed manifest is refused");
    check(!result.problems.empty(), "with a non-empty problem");
    check(ext::installed_packs(kStore).count("test.bad") == 0,
          "and the store gains no entry for the id");
}

void test_untrusted_origin_refused()
{
    const auto source = write_pack("user", manifest("test.user", 1));
    const ext::InstallResult result = ext::install_extension(source, kStore, ext::Origin::User);
    check(!result.ok(), "an untrusted origin is refused");
    check(contains(result.problems, "not trusted"), "the reason names the untrusted origin");
    check(ext::installed_packs(kStore).count("test.user") == 0, "and the store gains no entry");
}

void test_permission_shortfall_refused()
{
    // A Pack still contributes a shader (`execute`), but the User origin does
    // not grant it. The install reports the shortfall alongside the trust
    // refusal; both stem from the User origin, the one execute-denied origin in
    // the current Pack model.
    const auto source = write_pack("short", manifest("test.short", 1));
    const ext::InstallResult result = ext::install_extension(source, kStore, ext::Origin::User);
    check(!result.ok(), "a permission shortfall is refused");
    check(contains(result.problems, "permissions exceed"),
          "the reason names the permission shortfall");
    check(ext::installed_packs(kStore).count("test.short") == 0, "and the store gains no entry");
}

void test_reinstall_overwrites()
{
    const auto source = write_pack("again", manifest("test.again", 1));
    check(ext::install_extension(source, kStore, ext::Origin::Builtin).ok(),
          "the first install of an id+version succeeds");
    check(ext::install_extension(source, kStore, ext::Origin::Builtin).ok(),
          "reinstalling the same id+version succeeds");
    const auto packs = ext::installed_packs(kStore);
    check(packs.count("test.again") == 1, "still one id entry");
    if (packs.count("test.again") == 1) {
        check(packs.at("test.again").size() == 1, "still one version entry");
    }
}

void test_two_versions_coexist()
{
    check(ext::install_extension(write_pack("ver-1", manifest("test.versions", 1)), kStore,
                                 ext::Origin::Builtin)
                  .ok(),
          "version 1 installs");
    check(ext::install_extension(write_pack("ver-2", manifest("test.versions", 2)), kStore,
                                 ext::Origin::Builtin)
                  .ok(),
          "version 2 installs");
    const auto packs = ext::installed_packs(kStore);
    check(packs.count("test.versions") == 1, "one id entry");
    if (packs.count("test.versions") == 1) {
        check(packs.at("test.versions").size() == 2, "two versions of the id coexist");
        check(packs.at("test.versions").count("1") == 1, "version 1 present");
        check(packs.at("test.versions").count("2") == 1, "version 2 present");
    }
}

void test_native_extension_installs()
{
    // A native extension installs for Developer (visible, load-gated later)
    // and is refused for User, the one install-refused native origin.
    const auto source = write_native("native", native_manifest("test.native"));
    const ext::InstallResult developer =
            ext::install_extension(source, kStore, ext::Origin::Developer);
    check(developer.ok(), "a Developer native extension installs");
    if (developer.installed) {
        check(std::filesystem::exists(*developer.installed / "extension.toml"),
              "the installed root holds the native manifest");
        check(std::filesystem::exists(*developer.installed / "libfixture.so"),
              "and the library placeholder");
    }
    check(ext::installed_packs(kStore).count("test.native") == 1,
          "the native id appears in installed_packs");

    const ext::InstallResult user = ext::install_extension(source, kStore, ext::Origin::User);
    check(!user.ok(), "a User native extension is refused");
    check(contains(user.problems, "not trusted"), "the refusal names the untrusted origin");
}

void test_malformed_native_manifest_refused()
{
    // A native manifest that fails the schema's own validation (a non-
    // namespaced id) is refused before anything is copied.
    const auto source = write_native("native-bad", native_manifest("badid"));
    const ext::InstallResult result =
            ext::install_extension(source, kStore, ext::Origin::Developer);
    check(!result.ok(), "a malformed native manifest is refused");
    check(ext::installed_packs(kStore).count("badid") == 0,
          "and the store gains no entry for the id");
}

void test_neither_manifest_refused()
{
    // A directory holding neither extension.toml nor effect.toml is refused.
    const std::filesystem::path dir = kBase / "empty";
    std::filesystem::create_directories(dir);
    const ext::InstallResult result = ext::install_extension(dir, kStore, ext::Origin::Builtin);
    check(!result.ok(), "a directory with no manifest is refused");
    check(contains(result.problems, "no extension.toml or effect.toml"),
          "the refusal names the missing manifest");
}

void test_native_origin_recorded()
{
    // A native install records its origin in the store's origins.json; a
    // reinstall under a different origin updates the record (latest wins), and
    // a second id's record survives the later write.
    const auto source = write_native("orig-dev", native_manifest("test.origin"));
    check(ext::install_extension(source, kStore, ext::Origin::Developer).ok(),
          "a Developer native install succeeds");
    {
        const auto origins = ext::read_origins(kStore);
        check(origins.count("test.origin") == 1, "the id's origin is recorded");
        if (origins.count("test.origin") == 1) {
            check(origins.at("test.origin") == ext::Origin::Developer, "as Developer");
        }
    }

    check(ext::install_extension(source, kStore, ext::Origin::Curated).ok(),
          "reinstalling the same id as Curated succeeds");
    {
        const auto origins = ext::read_origins(kStore);
        check(origins.count("test.origin") == 1, "the record still has the id");
        if (origins.count("test.origin") == 1) {
            check(origins.at("test.origin") == ext::Origin::Curated,
                  "and the latest install's origin wins");
        }
    }

    const auto curated = write_native("orig-cur", native_manifest("test.origin2"));
    check(ext::install_extension(curated, kStore, ext::Origin::Curated).ok(),
          "a second Curated native install succeeds");
    {
        const auto origins = ext::read_origins(kStore);
        check(origins.count("test.origin2") == 1, "the second id's origin is recorded");
        if (origins.count("test.origin2") == 1) {
            check(origins.at("test.origin2") == ext::Origin::Curated, "as Curated");
        }
        check(origins.at("test.origin") == ext::Origin::Curated,
              "the first id's record survives the second write");
    }
}

void test_data_only_native_installs()
{
    // An entryless manifest (a data-only extension) installs for a trusted
    // origin; install never loads a library, so validation alone gates it. The
    // stray placeholder library write_native adds is irrelevant to the copy.
    const auto source = write_native("data-only", data_only_native_manifest("test.dataonly"));
    const ext::InstallResult result = ext::install_extension(source, kStore, ext::Origin::Builtin);
    check(result.ok(), "a data-only native extension installs");
    if (result.installed) {
        check(std::filesystem::exists(*result.installed / "extension.toml"),
              "the installed root holds the manifest");
    }
    check(ext::installed_packs(kStore).count("test.dataonly") == 1,
          "the data-only id appears in installed_packs");
}

void test_source_recorded()
{
    // A Dir install with the default empty source records the absolute source
    // path, so a config export can reproduce the install. An explicit source is
    // recorded verbatim instead.
    const auto source = write_native("sourced", native_manifest("test.sourced"));
    check(ext::install_extension(source, kStore, ext::Origin::Developer).ok(),
          "a Developer native install succeeds");

    const auto sources = ext::read_sources(kStore);
    check(sources.count("test.sourced") == 1, "the id's source is recorded");
    if (sources.count("test.sourced") == 1) {
        check(sources.at("test.sourced").type == ext::SourceType::Dir, "as a Dir source");
        check(sources.at("test.sourced").url == std::filesystem::absolute(source).string(),
              "with the absolute install path (empty url normalized)");
    }

    // An explicit source is recorded as given, not re-normalized.
    const auto explicit_source = write_native("explicit", native_manifest("test.explicit"));
    const ext::InstallSource explicit_record{ ext::SourceType::Git, "https://example.com/x.git" };
    check(ext::install_extension(explicit_source, kStore, ext::Origin::Developer, explicit_record)
                  .ok(),
          "an install with an explicit source succeeds");
    const auto re_read = ext::read_sources(kStore);
    check(re_read.count("test.explicit") == 1, "the explicit source is recorded");
    if (re_read.count("test.explicit") == 1) {
        check(re_read.at("test.explicit").type == ext::SourceType::Git,
              "the explicit Git type wins");
        check(re_read.at("test.explicit").url == "https://example.com/x.git",
              "the explicit url is preserved");
    }
}

} // namespace

int main()
{
    std::filesystem::remove_all(kBase); // clear a stale run
    std::filesystem::create_directories(kBase);

    test_trust_model();
    test_valid_pack_installs();
    test_malformed_manifest_refused();
    test_untrusted_origin_refused();
    test_permission_shortfall_refused();
    test_reinstall_overwrites();
    test_two_versions_coexist();
    test_native_extension_installs();
    test_malformed_native_manifest_refused();
    test_neither_manifest_refused();
    test_native_origin_recorded();
    test_data_only_native_installs();
    test_source_recorded();

    std::filesystem::remove_all(kBase);

    std::printf("%d checks, %d failures\n", checks, failures);
    return failures == 0 ? 0 : 1;
}
