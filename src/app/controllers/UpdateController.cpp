// SPDX-License-Identifier: GPL-3.0-or-later
// SPDX-FileCopyrightText: 2026 Genesis-0 contributors

#include "app/controllers/UpdateController.h"

#include <cstdlib>
#include <fstream>
#include <string>
#include <utility>

#include <nlohmann/json.hpp>

#include <QEventLoop>
#include <QNetworkAccessManager>
#include <QNetworkReply>
#include <QNetworkRequest>
#include <QUrl>

#include "app/Version.h"

namespace upd = genesis::workspace::update;

namespace {

// How long the feed fetch may sit with no byte arriving before it is treated as
// a dead connection. The feed is a small JSON manifest, not a package, so this
// is far tighter than a package download's budget - a user-initiated "Check
// now" should not hang the UI thread long.
constexpr int kFeedTransferTimeoutMs = 10 * 1000;

// The settings file inside the config dir that carries the persisted feed URL.
constexpr const char *kFeedFile = "updates.json";

// The environment variable that overrides the persisted feed URL.
constexpr const char *kFeedEnvVar = "GENESIS_UPDATE_FEED";

// The real feed fetch: one QNetworkAccessManager GET over the feed URL, on the
// calling thread (the UI thread for a user-initiated check). Returns the body,
// or nullopt on any transport error or timeout.
std::optional<std::string> http_fetch(const std::string &url)
{
    QNetworkAccessManager manager;
    QNetworkRequest request{ QUrl(QString::fromStdString(url)) };
    request.setAttribute(QNetworkRequest::RedirectPolicyAttribute,
                         QNetworkRequest::NoLessSafeRedirectPolicy);
    request.setTransferTimeout(kFeedTransferTimeoutMs);

    QNetworkReply *reply = manager.get(request);
    if (reply == nullptr) {
        return std::nullopt;
    }

    QEventLoop loop;
    QObject::connect(reply, &QNetworkReply::finished, &loop, &QEventLoop::quit);
    loop.exec();

    if (reply->error() != QNetworkReply::NoError) {
        reply->deleteLater();
        return std::nullopt;
    }
    const QByteArray body = reply->readAll();
    reply->deleteLater();
    return std::string(body.constData(), static_cast<std::size_t>(body.size()));
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
      fetcher_([](const std::string &url) { return http_fetch(url); }),
      verifier_([](const upd::Manifest &manifest) {
          return upd::verify_manifest(manifest, upd::kReleasePublicKey);
      })
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

void UpdateController::clearResult()
{
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
