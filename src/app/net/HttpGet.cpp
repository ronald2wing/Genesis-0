// SPDX-License-Identifier: GPL-3.0-or-later
// SPDX-FileCopyrightText: 2026 Genesis-0 contributors

#include "app/net/HttpGet.h"

#include <cstddef>

#include <QByteArray>
#include <QEventLoop>
#include <QNetworkAccessManager>
#include <QNetworkReply>
#include <QNetworkRequest>
#include <QString>
#include <QUrl>

namespace genesis::app {

std::optional<std::string> http_get(const std::string &url, int timeout_ms)
{
    // The manager lives on this thread - the one that called http_get - so its
    // event loop is the one `exec()` runs. Constructing it here means the
    // helper has no thread affinity of its own; only the call does.
    QNetworkAccessManager manager;
    QNetworkRequest request{ QUrl(QString::fromStdString(url)) };
    request.setAttribute(QNetworkRequest::RedirectPolicyAttribute,
                         QNetworkRequest::NoLessSafeRedirectPolicy);
    request.setTransferTimeout(timeout_ms);

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

} // namespace genesis::app
