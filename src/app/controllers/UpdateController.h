// SPDX-License-Identifier: GPL-3.0-or-later
// SPDX-FileCopyrightText: 2026 Genesis-0 contributors
//
// The studio shell's update controller. It checks a release feed, verifies it,
// and tells the user whether a newer build exists, and - once the user confirms
// - applies the update through the host apply path (src/workspace/update/).
// A "Check now" parses and verifies a manifest and compares its version against
// the running build's; a confirmed apply downloads + stages the package, then
// hands the swap to a detached helper that replaces the install and relaunches.
// The QML Settings section binds to the properties below and calls check() and
// apply(); the controller never touches QML.
//
// State machine. `state` is one of:
//   "disabled"         the build cannot check: either crypto is not compiled in
//                      (updater_enabled() is false) or the embedded release key
//                      is still all zeros. The two read as distinct reasons in
//                      statusText (disabledReason()).
//   "no-feed"          a check may run, but no feed URL is configured.
//   "up-to-date"       a check ran and the feed's version is not newer.
//   "update-available" a check ran and a newer version is signed; latestVersion
//                      and releaseNotes carry the manifest's version and notes.
//   "applying"         an apply was confirmed and the detached helper was
//                      spawned; the app is about to restart.
//   "error"            the fetch, parse, signature check or apply failed;
//                      statusText carries the reason.
//
// Everything is injectable (feed fetcher, signature verifier, the "may we
// check"/"may we apply" gate, the package downloader, the apply spawner and
// relauncher, and the install paths) so the full state machine is testable
// headlessly in a build without the updater.

#pragma once

#include <QObject>
#include <QString>

#include <filesystem>
#include <functional>
#include <optional>
#include <string>

#include "workspace/update/Apply.h"
#include "workspace/update/Manifest.h"
#include "workspace/update/Signature.h"
#include "workspace/update/Updater.h"

class UpdateController : public QObject
{
    Q_OBJECT

    // "disabled", "no-feed", "up-to-date", "update-available", "applying" or
    // "error".
    Q_PROPERTY(QString state READ state NOTIFY stateChanged)
    // The human-readable status line the Settings section shows.
    Q_PROPERTY(QString statusText READ statusText NOTIFY statusTextChanged)
    // The running build's version (the feed's version is compared against it).
    Q_PROPERTY(QString version READ version CONSTANT)
    // The feed manifest's version, set only when update-available.
    Q_PROPERTY(QString latestVersion READ latestVersion NOTIFY latestVersionChanged)
    // The feed manifest's release notes (informational, unsigned), when present.
    Q_PROPERTY(QString releaseNotes READ releaseNotes NOTIFY releaseNotesChanged)
    // The persisted feed URL, editable from Settings.
    Q_PROPERTY(QString feedUrl READ feedUrl WRITE setFeedUrl NOTIFY feedUrlChanged)
    // The URL a check actually uses: GENESIS_UPDATE_FEED overrides the setting.
    Q_PROPERTY(QString effectiveFeedUrl READ effectiveFeedUrl NOTIFY feedUrlChanged)
    // True while a check's fetch is in flight.
    Q_PROPERTY(bool checking READ checking NOTIFY checkingChanged)

public:
    // Where the feed manifest's JSON body comes from. Tests inject a fake; the
    // default is a short-timeout Qt GET in the .cpp.
    using FeedFetcher = std::function<std::optional<std::string>(const std::string &url)>;

    // The signature check over a parsed manifest. Tests inject a test fake so
    // the crypto-free path is exercised in a build without the updater; the
    // default binds verify_manifest against the embedded release key.
    using Verifier = std::function<genesis::workspace::update::VerifyResult(
            const genesis::workspace::update::Manifest &)>;

    // The "may we check" gate: returns an empty string when a check may run, or
    // the human-readable reason it may not. Tests inject "" to reach the feed
    // path in a build without the updater; the default is disabledReason().
    using Gate = std::function<QString()>;

    // The "may we apply" gate: returns an empty string when an apply may run, or
    // the human-readable reason it may not. The same disabled reasons as Gate,
    // but injected separately so a test can allow checking while forbidding
    // applying; the default is disabledReason().
    using ApplyGate = std::function<QString()>;

    // Downloads + stages the retained manifest's package, returning the staged
    // version directory ready to swap (or nullopt, with the reason surfaced in
    // statusText). Tests inject a fake that plants a directory; the default
    // runs the host install() pipeline (verify -> download -> digest -> stage)
    // through a real Qt transfer.
    using Downloader = std::function<std::optional<std::filesystem::path>(
            const genesis::workspace::update::Manifest &)>;

    // The detached-helper spawner and the relauncher, matching the host apply
    // path's seams. Tests inject fakes so nothing forks; the defaults are the
    // real spawn_detached_helper / relaunch_binary.
    using Spawner = genesis::workspace::update::Spawner;
    using Relauncher = genesis::workspace::update::Relauncher;

    // `runningVersion` overrides the build's version for tests; empty reads the
    // compile-time genesis::app::kAppVersion.
    explicit UpdateController(QString runningVersion = QString(), QObject *parent = nullptr);

    QString state() const { return state_; }
    QString statusText() const { return statusText_; }
    QString version() const { return runningVersion_; }
    QString latestVersion() const { return latestVersion_; }
    QString releaseNotes() const { return releaseNotes_; }
    QString feedUrl() const { return feedUrl_; }
    void setFeedUrl(const QString &url);
    QString effectiveFeedUrl() const;
    bool checking() const { return checking_; }

    // ---- Injection points (tests swap these in before a check). ----

    void setFetcher(FeedFetcher fetcher) { fetcher_ = std::move(fetcher); }
    void setVerifier(Verifier verifier) { verifier_ = std::move(verifier); }
    void setGate(Gate gate) { gate_ = std::move(gate); }
    // Where the feed URL persists (<dir>/updates.json); empty disables
    // persistence (a headless test that does not care). Loads any saved URL.
    void setConfigDir(const std::filesystem::path &dir);

    // ---- Injection points (tests swap these in before an apply). ----

    void setApplyGate(ApplyGate gate) { applyGate_ = std::move(gate); }
    void setDownloader(Downloader downloader) { downloader_ = std::move(downloader); }
    void setSpawner(Spawner spawner) { spawner_ = std::move(spawner); }
    void setRelauncher(Relauncher relauncher) { relauncher_ = std::move(relauncher); }
    // The update store root (where the host stages versions and keeps markers);
    // empty until the app wires it from its data location.
    void setApplyRoot(const std::filesystem::path &root) { applyRoot_ = root; }
    // The running install's directory, replaced by the staged version.
    void setInstallTarget(const std::filesystem::path &target) { installTarget_ = target; }
    // The executable relaunched after the swap (empty = do not relaunch).
    void setRelaunchBinary(const std::filesystem::path &binary) { relaunchBinary_ = binary; }

    // ---- The two disabled reasons, distinct so the UI (and the tests) can
    // tell a build without the updater from an enabled build whose release key
    // is still all zeros. ----

    static QString buildDisabledReason();
    static QString keyNotConfiguredReason();

    // The real compile-time gate: empty when a check may run, otherwise the
    // matching disabled reason.
    QString disabledReason() const;

    // Fetches the configured feed, parses and verifies the manifest, and
    // compares its version against the running build's, setting the state above.
    // Refuses with a clear reason instead of fetching when disabled or
    // no-feed. Synchronous (the Settings "Check now" runs on the UI thread);
    // the feed is a small JSON manifest fetched with a short timeout.
    Q_INVOKABLE void check();

    // Applies the update a successful check found, once the user confirms.
    // `confirm` must be true - the API and QML surface this as the user's
    // explicit consent to download and replace the running install; a false
    // value refuses. Refuses with a clear reason when disabled, when no check
    // has found a newer version, or when the package cannot be staged or the
    // detached helper cannot be spawned. On success the helper performs the
    // swap and relaunch after this process exits. Synchronous like check();
    // the package download runs on the calling thread (a user-initiated apply).
    Q_INVOKABLE void apply(bool confirm);

signals:
    void stateChanged();
    void statusTextChanged();
    void latestVersionChanged();
    void releaseNotesChanged();
    void feedUrlChanged();
    void checkingChanged();

private:
    // Re-derives the static state (disabled / no-feed) after a config change,
    // without touching the result a check may have produced otherwise.
    void refreshStaticState();
    void clearResult();
    void setState(const QString &value);
    void setStatusText(const QString &value);
    void setLatestVersion(const QString &value);
    void setReleaseNotes(const QString &value);
    void setChecking(bool value);

    QString runningVersion_;
    QString state_;
    QString statusText_;
    QString latestVersion_;
    QString releaseNotes_;
    QString feedUrl_;
    bool checking_ = false;

    std::filesystem::path configDir_;
    FeedFetcher fetcher_;
    Verifier verifier_;
    Gate gate_;

    // The manifest the last successful check found (nullopt until then). The
    // apply path stages exactly this version, never a stale or re-fetched one.
    std::optional<genesis::workspace::update::Manifest> manifest_;

    // Apply seams, all empty/nullopt until wired; the defaults live in the .cpp.
    ApplyGate applyGate_;
    Downloader downloader_;
    Spawner spawner_;
    Relauncher relauncher_;
    std::filesystem::path applyRoot_;
    std::filesystem::path installTarget_;
    std::filesystem::path relaunchBinary_;
};
