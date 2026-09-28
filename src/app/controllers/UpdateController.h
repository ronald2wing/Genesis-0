// SPDX-License-Identifier: GPL-3.0-or-later
// SPDX-FileCopyrightText: 2026 Genesis-0 contributors
//
// The studio shell's update controller: the notify-only surface that checks a
// release feed, verifies it, and tells the user whether a newer build exists.
// It downloads and applies nothing - the platform channel owns updates
// (docs/decisions/auto-update.md §3(d)); a "Check now" only parses and verifies
// a manifest, then compares its version against the running build's. The QML
// Settings section binds to the properties below and calls check(); the
// controller never touches QML.
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
//   "error"            the fetch, parse or signature check failed; statusText
//                      carries the reason.
//
// Everything the check needs is injectable (feed fetcher, signature verifier,
// and the "may we check" gate) so the full state machine is testable headlessly
// in a build without the updater.

#pragma once

#include <QObject>
#include <QString>

#include <filesystem>
#include <functional>
#include <optional>
#include <string>

#include "workspace/update/Manifest.h"
#include "workspace/update/Signature.h"

class UpdateController : public QObject
{
    Q_OBJECT

    // "disabled", "no-feed", "up-to-date", "update-available" or "error".
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
};
