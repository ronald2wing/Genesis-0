// SPDX-License-Identifier: GPL-3.0-or-later
// SPDX-FileCopyrightText: 2026 Genesis-0 contributors

#include <cstdint>
#include <cstdio>
#include <filesystem>
#include <fstream>
#include <span>
#include <string>
#include <string_view>
#include <vector>

#include "ai/Sha256.h"
#include "core/JsonGuard.h"
#include "workspace/update/Manifest.h"
#include "workspace/update/Signature.h"
#include "workspace/update/Updater.h"
#include "workspace/update/UpdateVersion.h"
#include "../support/Checks.h"

namespace {

using genesis::test::check;

namespace upd = genesis::workspace::update;

bool contains(const std::vector<std::string> &problems, const std::string &needle)
{
    for (const std::string &problem : problems) {
        if (problem.find(needle) != std::string::npos) {
            return true;
        }
    }
    return false;
}

std::filesystem::path scratch(std::string_view name)
{
    const std::filesystem::path dir =
            std::filesystem::temp_directory_path() / ("genesis-updater-" + std::string(name));
    std::error_code ec;
    std::filesystem::remove_all(dir, ec);
    std::filesystem::create_directories(dir, ec);
    return dir;
}

std::string sha256_of(std::string_view body)
{
    const std::vector<std::uint8_t> data(body.begin(), body.end());
    return genesis::ai::sha256_hex(data);
}

// A verifier stub that accepts anything - the hermetic tests exercise the
// crypto-free pipeline (download, digest, stage, swap, rollback), so the
// signature check is bypassed without needing OpenSSL in the default build.
const upd::Verifier kAccept = [](const upd::Manifest &) { return upd::VerifyResult::Verified; };

// A fetcher that streams `body` through the sink one byte at a time. When
// `interrupt_after` is set it stops short and reports a failed transfer, which
// exercises the interrupted-download cleanup.
upd::Fetcher canned(const std::string &body, std::size_t interrupt_after = std::string::npos)
{
    return [body, interrupt_after](const upd::Manifest &,
                                   const std::function<bool(std::span<const std::uint8_t>)> &sink) {
        const std::vector<std::uint8_t> data(body.begin(), body.end());
        const std::size_t limit = interrupt_after < data.size() ? interrupt_after : data.size();
        for (std::size_t i = 0; i < limit; ++i) {
            if (!sink(std::span<const std::uint8_t>(&data[i], 1))) {
                return false;
            }
        }
        return limit == data.size();
    };
}

upd::Manifest make_manifest(const std::string &version, const std::string &url,
                            const std::string &sha256)
{
    upd::Manifest manifest;
    manifest.version = version;
    manifest.url = url;
    manifest.sha256 = sha256;
    manifest.signature = std::string(128, '0');
    return manifest;
}

bool has_partial(const std::filesystem::path &versions_dir)
{
    std::error_code ec;
    for (const std::filesystem::directory_entry &entry :
         std::filesystem::directory_iterator(versions_dir, ec)) {
        if (entry.path().filename().string().starts_with(".tmp-")) {
            return true;
        }
    }
    return false;
}

// -- manifest parsing ---------------------------------------------------------

void test_parse_accepts_a_valid_manifest()
{
    const std::string json = "{\"version\":\"1.2.3\","
                             "\"url\":\"https://example/genesis-1.2.3.AppImage\","
                             "\"sha256\":\""
            + std::string(64, 'a')
            + "\","
              "\"signature\":\""
            + std::string(128, 'b') + "\"}";
    const upd::ParseResult parsed = upd::parse_manifest(json);
    check(parsed.ok(), "a well-formed manifest parses");
    if (parsed.manifest) {
        check(parsed.manifest->version == "1.2.3", "the version is read");
        check(parsed.manifest->url == "https://example/genesis-1.2.3.AppImage", "the url is read");
        check(parsed.manifest->sha256.size() == 64, "the sha256 is read");
        check(parsed.manifest->signature.size() == 128, "the signature is read");
    }
}

void test_parse_rejects_malformed_manifests()
{
    check(!upd::parse_manifest("not json at all").ok(), "garbage is refused");
    check(contains(upd::parse_manifest("not json at all").problems, "not valid JSON"),
          "garbage names the JSON problem");

    check(!upd::parse_manifest("{}").ok(), "an empty object is refused");
    check(contains(upd::parse_manifest("{}").problems, "no version"), "a missing version is named");

    const std::string no_sha = "{\"version\":\"1.0.0\",\"url\":\"https://example/x\","
                               "\"signature\":\""
            + std::string(128, 'c') + "\"}";
    check(contains(upd::parse_manifest(no_sha).problems, "no sha256"), "a missing sha256 is named");

    const std::string short_sha = "{\"version\":\"1.0.0\",\"url\":\"https://example/x\","
                                  "\"sha256\":\"abc\",\"signature\":\""
            + std::string(128, 'c') + "\"}";
    check(contains(upd::parse_manifest(short_sha).problems, "not 64 lowercase hex"),
          "a short sha256 is refused");

    const std::string upper_sha = "{\"version\":\"1.0.0\",\"url\":\"https://example/x\","
                                  "\"sha256\":\""
            + std::string(64, 'A') + "\",\"signature\":\"" + std::string(128, 'c') + "\"}";
    check(contains(upd::parse_manifest(upper_sha).problems, "not 64 lowercase hex"),
          "an uppercase sha256 is refused");

    const std::string short_sig = "{\"version\":\"1.0.0\",\"url\":\"https://example/x\","
                                  "\"sha256\":\""
            + std::string(64, 'a') + "\",\"signature\":\"ff\"}";
    check(contains(upd::parse_manifest(short_sig).problems, "not 128 lowercase hex"),
          "a short signature is refused");
}

void test_parse_refuses_oversized_or_deep_manifest()
{
    // A manifest past the 64 MiB document cap is refused on its raw bytes,
    // before the recursive parser could be handed a buffer sized to exhaust
    // memory.
    const std::string big(genesis::core::kMaxDocumentJsonBytes + 1, ' ');
    check(contains(upd::parse_manifest(big).problems, "64 MiB"),
          "an oversized manifest is refused by the size guard");

    // A manifest nesting past 512 levels is refused the same way.
    std::string deep;
    for (int i = 0; i < 600; ++i) {
        deep += '[';
    }
    for (int i = 0; i < 600; ++i) {
        deep += ']';
    }
    check(contains(upd::parse_manifest(deep).problems, "512"),
          "an over-nested manifest is refused by the depth guard");
}

// -- gating -------------------------------------------------------------------

void test_disabled_or_enabled_is_clean()
{
    if (upd::updater_enabled()) {
        check(true, "an enabled build reports enabled");
        return;
    }
    const upd::Manifest manifest =
            make_manifest("1.0.0", "https://example/x", std::string(64, 'a'));
    check(!upd::updater_enabled(), "the default build reports disabled");
    check(upd::verify_manifest(manifest, upd::kReleasePublicKey) == upd::VerifyResult::Unavailable,
          "verify_manifest reports unavailable when crypto is off");

    // The real-key verifier resolves to Unavailable, so install refuses before
    // any byte is fetched - clean disabled behaviour, not a crash.
    const auto refused = upd::install(
            scratch("disabled"), manifest,
            [](const upd::Manifest &m) { return upd::verify_manifest(m, upd::kReleasePublicKey); },
            canned("whatever"));
    check(!refused.ok(), "install refuses when crypto is unavailable");
    check(contains(refused.problems, "disabled"), "the refusal names the disabled crypto");

    // An empty verifier refuses too: signature, or do not self-update.
    const auto no_verifier =
            upd::install(scratch("noverifier"), manifest, upd::Verifier{ }, canned("whatever"));
    check(!no_verifier.ok(), "install refuses without a verifier");
    check(contains(no_verifier.problems, "no signature verifier"),
          "the refusal names the missing verifier");
}

// -- install pipeline ---------------------------------------------------------

void test_install_stages_and_swaps()
{
    const std::filesystem::path root = scratch("install");
    const std::string body = "the release payload";
    const upd::Manifest manifest =
            make_manifest("1.0.0", "https://example/genesis-1.0.0.AppImage", sha256_of(body));

    const upd::InstallResult result = upd::install(root, manifest, kAccept, canned(body));
    check(result.ok(), "a verified manifest with a matching digest installs");
    check(result.installed.has_value(), "and returns the staged file");
    if (result.installed) {
        check(std::filesystem::exists(*result.installed), "the staged file exists");
        std::ifstream in(*result.installed, std::ios::binary);
        const std::string read{ std::istreambuf_iterator<char>(in),
                                std::istreambuf_iterator<char>() };
        check(read == body, "the staged file holds the downloaded bytes");
    }
    check(upd::current_version(root) == "1.0.0", "current points at the new version");
    check(upd::pending_version(root) == "1.0.0", "the new version is pending");
    check(!upd::confirmed_version(root).has_value(),
          "nothing is confirmed before the new version runs");
}

void test_digest_mismatch_cleans_up()
{
    const std::filesystem::path root = scratch("mismatch");
    const upd::Manifest manifest =
            make_manifest("1.0.0", "https://example/genesis-1.0.0.AppImage", sha256_of("other"));

    const upd::InstallResult result =
            upd::install(root, manifest, kAccept, canned("the real payload"));
    check(!result.ok(), "a digest mismatch is refused");
    check(contains(result.problems, "do not match"), "the refusal names the digest mismatch");
    check(!upd::current_version(root).has_value(), "current is not swapped on a mismatch");
    check(!upd::pending_version(root).has_value(), "pending is not set on a mismatch");

    const std::filesystem::path versions = root / "versions";
    check(has_partial(versions) == false || !std::filesystem::exists(versions),
          "no partial file is left behind");
    check(!std::filesystem::exists(versions / "1.0.0"), "no version directory is left behind");
}

void test_interrupted_download_cleans_up()
{
    const std::filesystem::path root = scratch("interrupted");
    const upd::Manifest manifest = make_manifest("1.0.0", "https://example/genesis-1.0.0.AppImage",
                                                 sha256_of("the real payload"));

    const upd::InstallResult result = upd::install(
            root, manifest, kAccept, canned("the real payload", /*interrupt after*/ 4));
    check(!result.ok(), "an interrupted download is refused");
    check(contains(result.problems, "interrupted"), "the refusal names the interruption");
    const std::filesystem::path versions = root / "versions";
    check(has_partial(versions) == false || !std::filesystem::exists(versions),
          "no partial file is left behind");
}

void test_unsafe_version_rejected_before_fetch()
{
    const std::filesystem::path root = scratch("unsafe-version");
    bool fetched = false;
    const upd::Fetcher spy =
            [&fetched](const upd::Manifest &,
                       const std::function<bool(std::span<const std::uint8_t>)> &) {
                fetched = true;
                return true;
            };

    for (const char *version : { "../1.0.0", "a/b", "a\\b" }) {
        const upd::Manifest manifest = make_manifest(
                version, "https://example/genesis-1.0.0.AppImage", sha256_of("whatever"));
        fetched = false;
        const upd::InstallResult result = upd::install(root, manifest, kAccept, spy);
        check(!result.ok(), std::string("an unsafe version \"") + version + "\" is refused");
        check(contains(result.problems, "path segment"), "the refusal names the unsafe version");
        check(!fetched, "no fetch happens for an unsafe version");
    }
    check(!std::filesystem::exists(root / "versions" / ".." / ".."),
          "no directory escapes the versions root");
}

// -- confirmation and rollback ------------------------------------------------

void test_compare_versions_monotonic()
{
    check(upd::compare_versions("2.0.0", "1.0.0") > 0, "newer reads newer");
    check(upd::compare_versions("1.0.0", "2.0.0") < 0, "older reads older");
    check(upd::compare_versions("1.2.3", "1.2.3") == 0, "equal reads equal");
    check(upd::compare_versions("1.2", "1.2.0") == 0, "a missing tail reads zero");
    check(upd::compare_versions("1.2.10", "1.2.9") > 0, "numeric compare, not lexicographic");
    const std::string huge(30, '9');
    check(upd::compare_versions(huge, "1.0.0") > 0, "a component past uint64 range reads huge");
}

void test_monotonic_refuses_downgrade_and_replay()
{
    const std::filesystem::path root = scratch("monotonic");
    bool fetched = false;
    const upd::Fetcher spy = [&](const upd::Manifest &,
                                 const std::function<bool(std::span<const std::uint8_t>)> &) {
        fetched = true;
        return true;
    };

    // A fresh install with a supplied running version seeds the baseline from
    // it, so a manifest at the running version reads as a replay.
    const upd::Manifest equal =
            make_manifest("2.0.0", "https://example/genesis-2.0.0.AppImage", sha256_of("two"));
    const upd::InstallResult replay = upd::install(root, equal, kAccept, spy, "2.0.0");
    check(!replay.ok(), "a manifest at the running version is refused as replay");
    check(contains(replay.problems, "replay"), "the refusal names a replay");
    check(!fetched, "no fetch happens for a replayed version");

    // Older than the running version reads as a downgrade.
    fetched = false;
    const upd::Manifest older =
            make_manifest("1.9.0", "https://example/genesis-1.9.0.AppImage", sha256_of("one"));
    const upd::InstallResult downgrade = upd::install(root, older, kAccept, spy, "2.0.0");
    check(!downgrade.ok(), "a manifest older than the running version is refused");
    check(contains(downgrade.problems, "downgrade"), "the refusal names a downgrade");
    check(!fetched, "no fetch happens for a downgrade");

    // Newer is still accepted, and install() does not advance the record.
    const upd::Manifest newer =
            make_manifest("2.1.0", "https://example/genesis-2.1.0.AppImage", sha256_of("three"));
    const upd::InstallResult accepted =
            upd::install(root, newer, kAccept, canned("three"), "2.0.0");
    check(accepted.ok(), "a manifest newer than the running version installs");
    check(upd::current_version(root) == "2.1.0", "current points at the newer version");
    check(upd::last_seen_version(root) == "2.0.0", "install does not advance the last-seen record");
}

void test_monotonic_persists_across_calls()
{
    const std::filesystem::path root = scratch("persist");
    const upd::Manifest v1 =
            make_manifest("1.0.0", "https://example/genesis-1.0.0.AppImage", sha256_of("one"));
    check(upd::install(root, v1, kAccept, canned("one")).ok(),
          "version 1 installs on a fresh root");
    upd::confirm(root);
    check(upd::last_seen_version(root) == "1.0.0", "confirm records the last-seen version");

    const upd::Manifest v1_again =
            make_manifest("1.0.0", "https://example/genesis-1.0.0.AppImage", sha256_of("one"));
    const upd::InstallResult replay = upd::install(root, v1_again, kAccept, canned("one"));
    check(!replay.ok() && contains(replay.problems, "replay"),
          "the same version is refused as replay across calls");

    const upd::Manifest older =
            make_manifest("0.9.0", "https://example/genesis-0.9.0.AppImage", sha256_of("old"));
    const upd::InstallResult downgrade = upd::install(root, older, kAccept, canned("old"));
    check(!downgrade.ok() && contains(downgrade.problems, "downgrade"),
          "an older version is refused as a downgrade across calls");

    const upd::Manifest v2 =
            make_manifest("2.0.0", "https://example/genesis-2.0.0.AppImage", sha256_of("two"));
    check(upd::install(root, v2, kAccept, canned("two")).ok(), "a newer version still installs");
}

void test_monotonic_malformed_version_handling()
{
    const std::filesystem::path root = scratch("malformed");
    bool fetched = false;
    const upd::Fetcher spy = [&](const upd::Manifest &,
                                 const std::function<bool(std::span<const std::uint8_t>)> &) {
        fetched = true;
        return true;
    };

    // A version with no numeric content reads zero and is refused as a
    // downgrade against a seeded baseline - never accepted, never a crash.
    const upd::Manifest garbage =
            make_manifest("not-a-version", "https://example/genesis.AppImage", sha256_of("x"));
    const upd::InstallResult result = upd::install(root, garbage, kAccept, spy, "1.0.0");
    check(!result.ok(), "a non-numeric version is refused");
    check(contains(result.problems, "downgrade"), "the refusal reads a downgrade");
    check(!fetched, "no fetch happens for a non-numeric version");
}

void test_confirm_promotes_the_pending_version()
{
    const std::filesystem::path root = scratch("confirm");
    const upd::Manifest manifest =
            make_manifest("1.0.0", "https://example/genesis-1.0.0.AppImage", sha256_of("one"));
    check(upd::install(root, manifest, kAccept, canned("one")).ok(), "the first version installs");

    upd::confirm(root);
    check(upd::confirmed_version(root) == "1.0.0", "confirm records the version as known-good");
    check(!upd::pending_version(root).has_value(), "confirm clears the pending marker");
}

void test_rollback_restores_the_confirmed_version()
{
    const std::filesystem::path root = scratch("rollback");
    const upd::Manifest v1 =
            make_manifest("1.0.0", "https://example/genesis-1.0.0.AppImage", sha256_of("one"));
    check(upd::install(root, v1, kAccept, canned("one")).ok(), "version 1 installs");
    upd::confirm(root);

    const upd::Manifest v2 =
            make_manifest("2.0.0", "https://example/genesis-2.0.0.AppImage", sha256_of("two"));
    check(upd::install(root, v2, kAccept, canned("two")).ok(), "version 2 installs");
    check(upd::current_version(root) == "2.0.0", "current is version 2");
    check(upd::confirmed_version(root) == "1.0.0", "version 1 stays the confirmed baseline");

    check(upd::rollback(root), "rollback succeeds");
    check(upd::current_version(root) == "1.0.0", "current is restored to version 1");
    check(!upd::pending_version(root).has_value(), "rollback clears the pending marker");
    check(upd::confirmed_version(root) == "1.0.0", "the confirmed baseline is unchanged");
    // The v2 directory stays put - the running binary is never overwritten.
    check(std::filesystem::exists(root / "versions" / "2.0.0"),
          "version 2 is kept, not deleted, on rollback");
}

void test_rollback_without_a_baseline_fails()
{
    const std::filesystem::path root = scratch("fresh-rollback");
    check(!upd::rollback(root), "rollback with no confirmed baseline reports failure");
}

} // namespace

int main()
{
    test_parse_accepts_a_valid_manifest();
    test_parse_rejects_malformed_manifests();
    test_parse_refuses_oversized_or_deep_manifest();
    test_disabled_or_enabled_is_clean();
    test_install_stages_and_swaps();
    test_digest_mismatch_cleans_up();
    test_interrupted_download_cleans_up();
    test_unsafe_version_rejected_before_fetch();
    test_compare_versions_monotonic();
    test_monotonic_refuses_downgrade_and_replay();
    test_monotonic_persists_across_calls();
    test_monotonic_malformed_version_handling();
    test_confirm_promotes_the_pending_version();
    test_rollback_restores_the_confirmed_version();
    test_rollback_without_a_baseline_fails();

    return genesis::test::summary();
}
