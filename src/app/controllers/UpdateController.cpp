// SPDX-License-Identifier: GPL-3.0-or-later
// SPDX-FileCopyrightText: 2026 Genesis-0 contributors

#include "app/controllers/UpdateController.h"

#include <cstdint>
#include <cstdlib>
#include <fstream>
#include <span>
#include <string>
#include <utility>

#include <nlohmann/json.hpp>

#include <QEventLoop>
#include <QNetworkAccessManager>
#include <QNetworkReply>
#include <QNetworkRequest>
#include <QUrl>

#include "app/Version.h"
#include "app/net/HttpGet.h"

namespace upd = genesis::workspace::update;

namespace {

// How long the feed fetch may sit with no byte arriving before it is treated as
// a dead connection. The feed is a small JSON manifest, not a package, so this
// is far tighter than a package download's budget - a user-initiated "Check
// now" should not hang the UI thread long. It is the shared GET helper's
// default (app/net/HttpGet.h), so the feed fetch just calls the helper.

// The package download is a real artifact, so it gets a roomier budget; still
// bounded, because an apply runs synchronously on the UI thread.
constexpr int kPackageTransferTimeoutMs = 60 * 60 * 1000;

// The settings file inside the config dir that carries the persisted feed URL.
constexpr const char *kFeedFile = "updates.json";

// The environment variable that overrides the persisted feed URL.
constexpr const char *kFeedEnvVar = "GENESIS_UPDATE_FEED";

// Streams the manifest's package through `sink` (the host install's digest
// check), like the feed fetch but feeding bytes as they arrive. Returns false
// on any transport error or when the sink asks the transfer to stop (a digest
// mismatch, which the host then reports).
bool package_fetch(const upd::Manifest &manifest,
                   const std::function<bool(std::span<const std::uint8_t>)> &sink)
{
    QNetworkAccessManager manager;
    QNetworkRequest request{ QUrl(QString::fromStdString(manifest.url)) };
    request.setAttribute(QNetworkRequest::RedirectPolicyAttribute,
                         QNetworkRequest::NoLessSafeRedirectPolicy);
    request.setTransferTimeout(kPackageTransferTimeoutMs);

    QNetworkReply *reply = manager.get(request);
    if (reply == nullptr) {
        return false;
    }
    QObject::connect(reply, &QNetworkReply::readyRead, reply, [reply, &sink] {
        while (reply->bytesAvailable() > 0) {
            const QByteArray chunk = reply->read(64 * 1024);
            if (!sink(std::span(reinterpret_cast<const std::uint8_t *>(chunk.constData()),
                                static_cast<std::size_t>(chunk.size())))) {
                reply->abort();
                return;
            }
        }
    });

    QEventLoop loop;
    QObject::connect(reply, &QNetworkReply::finished, &loop, &QEventLoop::quit);
    loop.exec();
    const bool ok = reply->error() == QNetworkReply::NoError;
    reply->deleteLater();
    return ok;
}

// Reads the persisted feed URL, or nullopt when absent/unreadable/corrupt.
std::optional<std::string> read_feed_url(const std::filesystem::path &config)
{
    std::ifstream in(config / kFeedFile);
    if (!in) {
        return std::nullopt;
    }
    const nlohmann::json doc = nlohmann::json::parse(in, nullptr, false);
    if (doc.is_discarded() || !doc.is_object()) {
        return std::nullopt;
    }
    const auto it = doc.find("feedUrl");
    if (it == doc.end() || !it->is_string() || it->get<std::string>().empty()) {
        return std::nullopt;
    }
    return it->get<std::string>();
}

// Best-effort write, like the recents file: an unwritable config dir is not
// worth failing a setting change over.
void write_feed_url(const std::filesystem::path &config, std::string_view url)
{
    std::error_code ec;
    std::filesystem::create_directories(config, ec);
    if (ec) {
        return;
    }
    nlohmann::json doc = nlohmann::json::object();
    doc["feedUrl"] = std::string(url);
    std::ofstream out(config / kFeedFile);
    if (!out) {
        return;
    }
    out << doc.dump(2) << '\n';
}

} // namespace

UpdateController::UpdateController(QString runningVersion, QObject *parent)
    : QObject(parent),
      runningVersion_(runningVersion.isEmpty()
                              ? QString::fromLatin1(
                                        genesis::app::kAppVersion.data(),
                                        static_cast<qsizetype>(genesis::app::kAppVersion.size()))
                              : std::move(runningVersion)),
      fetcher_([](const std::string &url) { return genesis::app::http_get(url); }),
      verifier_([](const upd::Manifest &manifest) {
          return upd::verify_manifest(manifest, upd::kReleasePublicKey);
      }),
      // The real apply pipeline: download + stage through the host install()
      // (whose digest check runs inside a Qt transfer), then the detached
      // helper that swaps and relaunches. Both are seams so tests can swap in
      // fakes and exercise the controller headlessly.
      downloader_([this](const upd::Manifest &manifest) {
          // Seed the anti-downgrade record from the running version on a fresh
          // store, so a manifest at or below the running version is refused from
          // the first check (install() records it only when none is present).
          const upd::InstallResult staged = upd::install(
                  applyRoot_, manifest, verifier_, package_fetch, runningVersion_.toStdString());
          if (!staged.ok() || !staged.installed) {
              return std::optional<std::filesystem::path>{ };
          }
          // The version directory (versions/<v>/), the directory the apply
          // path swaps into place. v1 swaps a directory; unpacking an archive
          // package into it is the documented follow-on (auto-update.md §10).
          return std::optional<std::filesystem::path>{ staged.installed->parent_path() };
      }),
      spawner_([this](const upd::ApplyPlan &plan) {
          return upd::spawn_detached_helper(plan, relauncher_);
      }),
      relauncher_([](const std::filesystem::path &binary) { return upd::relaunch_binary(binary); })
{
    refreshStaticState();
}

QString UpdateController::buildDisabledReason()
{
    return QStringLiteral("disabled: crypto not compiled in (rebuild with GENESIS_UPDATER=ON)");
}

QString UpdateController::keyNotConfiguredReason()
{
    return QStringLiteral("disabled: release key not configured");
}

QString UpdateController::disabledReason() const
{
    if (!upd::updater_enabled()) {
        return buildDisabledReason();
    }
    if (!upd::release_key_configured()) {
        return keyNotConfiguredReason();
    }
    return QString();
}

QString UpdateController::effectiveFeedUrl() const
{
    if (const char *env = std::getenv(kFeedEnvVar); env != nullptr && *env != '\0') {
        return QString::fromUtf8(env);
    }
    return feedUrl_;
}

void UpdateController::setFeedUrl(const QString &url)
{
    if (feedUrl_ == url) {
        return;
    }
    feedUrl_ = url;
    if (!configDir_.empty()) {
        write_feed_url(configDir_, url.toStdString());
    }
    emit feedUrlChanged();
    refreshStaticState();
}

void UpdateController::setConfigDir(const std::filesystem::path &dir)
{
    configDir_ = dir;
    if (configDir_.empty()) {
        return;
    }
    if (const std::optional<std::string> saved = read_feed_url(configDir_)) {
        const QString value = QString::fromStdString(*saved);
        if (feedUrl_ != value) {
            feedUrl_ = value;
            emit feedUrlChanged();
        }
    }
    refreshStaticState();
}

void UpdateController::refreshStaticState()
{
    if (const QString reason = gate_ ? gate_() : disabledReason(); !reason.isEmpty()) {
        clearResult();
        setState(QStringLiteral("disabled"));
        setStatusText(reason);
        return;
    }
    if (effectiveFeedUrl().isEmpty()) {
        clearResult();
        setState(QStringLiteral("no-feed"));
        setStatusText(QStringLiteral("no update feed configured"));
    }
    // Otherwise (enabled, feed configured): leave the last check's result
    // untouched; before the first check the UI shows an empty status line.
}

void UpdateController::check()
{
    if (const QString reason = gate_ ? gate_() : disabledReason(); !reason.isEmpty()) {
        clearResult();
        setState(QStringLiteral("disabled"));
        setStatusText(reason);
        return;
    }
    const std::string url = effectiveFeedUrl().toStdString();
    if (url.empty()) {
        clearResult();
        setState(QStringLiteral("no-feed"));
        setStatusText(QStringLiteral("no update feed configured"));
        return;
    }

    setChecking(true);
    const std::optional<std::string> body = fetcher_(url);
    if (!body) {
        setChecking(false);
        clearResult();
        setState(QStringLiteral("error"));
        setStatusText(QStringLiteral("could not fetch the update feed"));
        return;
    }

    const upd::ParseResult parsed = upd::parse_manifest(*body);
    if (!parsed.ok()) {
        setChecking(false);
        clearResult();
        setState(QStringLiteral("error"));
        setStatusText(QStringLiteral("the update feed is invalid: ")
                      + QString::fromStdString(parsed.problems.empty() ? "unreadable"
                                                                       : parsed.problems.front()));
        return;
    }

    if (verifier_(*parsed.manifest) != upd::VerifyResult::Verified) {
        setChecking(false);
        clearResult();
        setState(QStringLiteral("error"));
        setStatusText(QStringLiteral("the update feed failed signature verification"));
        return;
    }
    setChecking(false);

    const int comparison =
            genesis::app::compare_versions(parsed.manifest->version, runningVersion_.toStdString());
    if (comparison > 0) {
        manifest_ = *parsed.manifest;
        setState(QStringLiteral("update-available"));
        setLatestVersion(QString::fromStdString(parsed.manifest->version));
        setReleaseNotes(QString::fromStdString(parsed.manifest->notes));
        setStatusText(QStringLiteral("version ") + QString::fromStdString(parsed.manifest->version)
                      + QStringLiteral(" is available"));
    } else {
        clearResult();
        setState(QStringLiteral("up-to-date"));
        setStatusText(QStringLiteral("up to date"));
    }
}

void UpdateController::apply(bool confirm)
{
    if (!confirm) {
        setStatusText(QStringLiteral("the update was not confirmed"));
        return;
    }

    // The gate is shared with the host apply(), so a permissive test gate (or
    // the real disabled reason) is applied exactly once, at the same point the
    // host would refuse.
    const std::function<std::string()> host_gate = [this] {
        return (applyGate_ ? applyGate_() : disabledReason()).toStdString();
    };
    const std::string reason = host_gate();
    if (!reason.empty()) {
        clearResult();
        setState(QStringLiteral("disabled"));
        setStatusText(QString::fromStdString(reason));
        return;
    }
    if (!manifest_) {
        setStatusText(QStringLiteral("no update available - run a check first"));
        return;
    }
    if (applyRoot_.empty() || installTarget_.empty()) {
        setState(QStringLiteral("error"));
        setStatusText(QStringLiteral("the update install location is not configured"));
        return;
    }
    if (!downloader_) {
        setState(QStringLiteral("error"));
        setStatusText(QStringLiteral("no package downloader is wired"));
        return;
    }

    const std::optional<std::filesystem::path> staged = downloader_(*manifest_);
    if (!staged) {
        setState(QStringLiteral("error"));
        setStatusText(QStringLiteral("could not download the update package"));
        return;
    }

    const upd::ApplyResult result =
            upd::apply(applyRoot_, installTarget_, *staged, relaunchBinary_, spawner_, host_gate);
    if (!result.ok()) {
        setState(QStringLiteral("error"));
        setStatusText(QStringLiteral("could not apply the update: ")
                      + QString::fromStdString(result.problems.empty() ? "unknown"
                                                                       : result.problems.front()));
        return;
    }

    setState(QStringLiteral("applying"));
    setStatusText(QStringLiteral("installing version ") + latestVersion_
                  + QStringLiteral(" - the app will restart"));
}

void UpdateController::clearResult()
{
    manifest_.reset();
    setLatestVersion(QString());
    setReleaseNotes(QString());
}

void UpdateController::setState(const QString &value)
{
    if (state_ == value) {
        return;
    }
    state_ = value;
    emit stateChanged();
}

void UpdateController::setStatusText(const QString &value)
{
    if (statusText_ == value) {
        return;
    }
    statusText_ = value;
    emit statusTextChanged();
}

void UpdateController::setLatestVersion(const QString &value)
{
    if (latestVersion_ == value) {
        return;
    }
    latestVersion_ = value;
    emit latestVersionChanged();
}

void UpdateController::setReleaseNotes(const QString &value)
{
    if (releaseNotes_ == value) {
        return;
    }
    releaseNotes_ = value;
    emit releaseNotesChanged();
}

void UpdateController::setChecking(bool value)
{
    if (checking_ == value) {
        return;
    }
    checking_ = value;
    emit checkingChanged();
}
