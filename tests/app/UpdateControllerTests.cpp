// SPDX-License-Identifier: GPL-3.0-or-later
// SPDX-FileCopyrightText: 2026 Genesis-0 contributors

// The update controller's suite: drives the notify-only state machine
// headlessly with injected fakes (a stub feed fetcher, a stub signature
// verifier and a "may we check" gate), so the whole flow is testable in a build
// without the updater runtime. No network and no GStreamer: the default Qt GET
// is never reached because every check injects its fetcher.

#include <cstdio>
#include <filesystem>
#include <optional>
#include <string>
#include <string_view>

#include <QCoreApplication>
#include <QString>

#include "app/Version.h"
#include "app/controllers/UpdateController.h"
#include "workspace/update/Signature.h"

namespace upd = genesis::workspace::update;

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

// A syntactically valid manifest whose digest/signature fields are well-formed
// (64/128 lowercase hex) but not actually verifiable - the test injects the
// verifier, so the bytes never matter.
std::string manifest_json(std::string_view version, std::string_view notes = { })
{
    std::string json = R"({"version":")";
    json += version;
    json += R"(","url":"https://example.invalid/genesis-0-)";
    json += version;
    json += ".tar.gz\",\"sha256\":\"";
    json += std::string(64, '0');
    json += "\",\"signature\":\"";
    json += std::string(128, '0');
    json += "\"";
    if (!notes.empty()) {
        json += ",\"notes\":\"";
        json += notes;
        json += "\"";
    }
    json += "}";
    return json;
}

// Opens an existing controller's gate and makes its verifier accept
// everything, leaving the running version and feed URL to the caller. (The
// controller is a QObject, so it cannot be returned by value.)
void make_checker(UpdateController &ctl)
{
    ctl.setGate([] { return QString(); });
    ctl.setVerifier([](const upd::Manifest &) { return upd::VerifyResult::Verified; });
}

void test_initial_disabled()
{
    UpdateController ctl;
    check(ctl.state() == "disabled", "a fresh controller starts disabled");
    check(ctl.statusText().startsWith("disabled:"), "the status text carries a disabled reason");
    // The reason matches the real compile-time gate: an enabled build whose key
    // is still all zeros reports the key reason, the default build the crypto
    // reason - they must never be conflated.
    const QString expected = upd::updater_enabled() ? UpdateController::keyNotConfiguredReason()
                                                    : UpdateController::buildDisabledReason();
    check(ctl.disabledReason() == expected, "disabledReason() matches the build configuration");
    check(ctl.statusText() == expected, "the initial statusText is the build's disabled reason");
}

void test_no_feed()
{
    UpdateController ctl;
    ctl.setGate([] { return QString(); });
    ctl.check();
    check(ctl.state() == "no-feed", "an open gate with no feed reads no-feed");
    check(ctl.statusText() == "no update feed configured", "the no-feed status text is set");
}

void test_up_to_date()
{
    UpdateController ctl(QString::fromStdString("1.0.0"));
    make_checker(ctl);
    ctl.setFeedUrl("https://example.invalid/feed.json");
    ctl.setFetcher(
            [](const std::string &) { return std::optional<std::string>(manifest_json("0.9.0")); });
    ctl.check();
    check(ctl.state() == "up-to-date", "an older feed version reads up-to-date");
    check(ctl.statusText() == "up to date", "the up-to-date status text is set");
    check(ctl.latestVersion().isEmpty(), "up-to-date leaves latestVersion empty");
    check(ctl.releaseNotes().isEmpty(), "up-to-date leaves releaseNotes empty");
}

void test_update_available()
{
    UpdateController ctl(QString::fromStdString("1.0.0"));
    make_checker(ctl);
    ctl.setFeedUrl("https://example.invalid/feed.json");
    ctl.setFetcher([](const std::string &) {
        return std::optional<std::string>(manifest_json("2.0.0", "fixes the big bug"));
    });
    ctl.check();
    check(ctl.state() == "update-available", "a newer feed version reads update-available");
    check(ctl.latestVersion() == "2.0.0", "latestVersion carries the feed version");
    check(ctl.releaseNotes() == "fixes the big bug", "releaseNotes carry the unsigned notes field");
    check(ctl.statusText().contains("2.0.0"), "the status text names the available version");
}

void test_fetch_error()
{
    UpdateController ctl(QString::fromStdString("1.0.0"));
    make_checker(ctl);
    ctl.setFeedUrl("https://example.invalid/feed.json");
    ctl.setFetcher([](const std::string &) { return std::optional<std::string>(); });
    ctl.check();
    check(ctl.state() == "error", "a failed fetch reads error");
    check(ctl.statusText() == "could not fetch the update feed",
          "the fetch-error status text is set");
}

void test_invalid_manifest()
{
    UpdateController ctl(QString::fromStdString("1.0.0"));
    make_checker(ctl);
    ctl.setFeedUrl("https://example.invalid/feed.json");
    ctl.setFetcher(
            [](const std::string &) { return std::optional<std::string>("not json at all"); });
    ctl.check();
    check(ctl.state() == "error", "an unparseable feed reads error");
    check(ctl.statusText().contains("invalid"), "the status text names the parse failure");
}

void test_bad_signature()
{
    UpdateController ctl(QString::fromStdString("1.0.0"));
    make_checker(ctl);
    ctl.setFeedUrl("https://example.invalid/feed.json");
    ctl.setVerifier([](const upd::Manifest &) { return upd::VerifyResult::Rejected; });
    ctl.setFetcher(
            [](const std::string &) { return std::optional<std::string>(manifest_json("9.9.9")); });
    ctl.check();
    check(ctl.state() == "error", "a rejected signature reads error");
    check(ctl.statusText().contains("signature"), "the status text names the signature failure");
    check(ctl.latestVersion().isEmpty(), "a rejected manifest never surfaces its version");
}

void test_gate_blocks_even_with_feed()
{
    UpdateController ctl;
    ctl.setFeedUrl("https://example.invalid/feed.json");
    // The gate is deliberately left as the real compile-time gate (no
    // injection), so a check refuses before fetching regardless of the URL.
    ctl.check();
    check(ctl.state() == "disabled", "the real gate refuses the check");
    check(ctl.statusText().startsWith("disabled:"), "the refusal names a disabled reason");
}

void test_feed_url_roundtrip_and_persistence()
{
    const std::filesystem::path dir =
            std::filesystem::temp_directory_path() / "genesis-update-test";
    std::filesystem::remove_all(dir);

    {
        UpdateController ctl;
        ctl.setConfigDir(dir);
        ctl.setFeedUrl("https://example.invalid/feed.json");
    }
    // A second controller reading the same config dir restores the URL.
    UpdateController ctl;
    ctl.setConfigDir(dir);
    check(ctl.feedUrl() == "https://example.invalid/feed.json",
          "the feed URL survives a reload from the config dir");

    std::filesystem::remove_all(dir);
}

void test_env_override()
{
    UpdateController ctl;
    ctl.setFeedUrl("https://example.invalid/configured.json");

    check(ctl.effectiveFeedUrl() == "https://example.invalid/configured.json",
          "without the env var the setting is used");

    qputenv("GENESIS_UPDATE_FEED", "https://example.invalid/env.json");
    check(ctl.effectiveFeedUrl() == "https://example.invalid/env.json",
          "the env var overrides the setting");
    qunsetenv("GENESIS_UPDATE_FEED");
    check(ctl.effectiveFeedUrl() == "https://example.invalid/configured.json",
          "removing the env var restores the setting");
}

void test_compare_versions()
{
    using genesis::app::compare_versions;
    check(compare_versions("2.0.0", "1.0.0") > 0, "newer reads newer");
    check(compare_versions("1.0.0", "2.0.0") < 0, "older reads older");
    check(compare_versions("1.2.3", "1.2.3") == 0, "equal reads equal");
    check(compare_versions("1.2", "1.2.0") == 0, "a missing tail reads zero");
    check(compare_versions("v1.2.3", "1.2.3") == 0, "a v prefix is skipped");
    check(compare_versions("1.2.10", "1.2.9") > 0, "numeric compare, not lexicographic");

    // A component longer than a uint64 holds saturates instead of wrapping:
    // a ridiculous version still reads newer than any sane one, never older.
    const std::string huge(30, '9');
    check(compare_versions(huge, "1.0.0") > 0,
          "a component past uint64 range reads huge, not wrapped");
    check(compare_versions("1.0.0", huge) < 0,
          "the same huge component reads newer from either side");
    check(compare_versions(huge, huge) == 0, "two huge components compare equal");
}

void test_release_key_still_zeroed()
{
    // Until a real key is minted the embedded key is all zeros, so even an
    // enabled build reports no key and refuses checks - the test pins that the
    // distinction stays observable.
    check(!upd::release_key_configured(), "the embedded release key is not yet configured");
}

} // namespace

int main(int argc, char **argv)
{
    QCoreApplication app(argc, argv);

    test_initial_disabled();
    test_no_feed();
    test_up_to_date();
    test_update_available();
    test_fetch_error();
    test_invalid_manifest();
    test_bad_signature();
    test_gate_blocks_even_with_feed();
    test_feed_url_roundtrip_and_persistence();
    test_env_override();
    test_compare_versions();
    test_release_key_still_zeroed();

    std::printf("%d checks, %d failures\n", checks, failures);
    return failures == 0 ? 0 : 1;
}
