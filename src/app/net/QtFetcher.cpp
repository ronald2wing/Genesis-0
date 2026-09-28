// SPDX-License-Identifier: GPL-3.0-or-later
// SPDX-FileCopyrightText: 2026 Genesis-0 contributors

#include "app/net/QtFetcher.h"

#include <cstddef>

#include <QByteArray>
#include <QEventLoop>
#include <QNetworkAccessManager>
#include <QNetworkReply>
#include <QNetworkRequest>
#include <QString>
#include <QTimer>
#include <QUrl>

namespace {

// How long a transfer may sit with no byte arriving before it is treated as a
// dead connection and failed (so a stalled socket cannot hold the worker
// forever). The timeout is per-transfer, not per-download: any progress resets
// it, so a slow but moving link is never cut off.
constexpr int kTransferTimeoutMs = 5 * 60 * 1000;

// How often the cancel watchdog polls the caller's flag while bytes stream.
constexpr int kCancelPollMs = 50;

std::span<const std::uint8_t> as_bytes(const QByteArray &chunk)
{
    return std::span<const std::uint8_t>(reinterpret_cast<const std::uint8_t *>(chunk.constData()),
                                         static_cast<std::size_t>(chunk.size()));
}

} // namespace

QtFetcher::QtFetcher(const std::atomic<bool> *cancel) : cancel_(cancel) { }

bool QtFetcher::operator()(const genesis::ai::ModelEntry &entry, const Sink &sink) const
{
    // Mirror before upstream (ai-provider.md §3(c)); a mirror that fails
    // falls through, a sink-stop or a cancel does not.
    std::vector<std::string> urls;
    if (!entry.mirror.empty()) {
        urls.push_back(entry.mirror);
    }
    if (!entry.url.empty()) {
        urls.push_back(entry.url);
    }
    for (const std::string &url : urls) {
        const Transfer outcome = transfer(url, sink);
        if (outcome == Transfer::Complete) {
            return true;
        }
        if (outcome != Transfer::Failed) {
            return false;
        }
    }
    return false;
}

QtFetcher::Transfer QtFetcher::transfer(const std::string &url, const Sink &sink) const
{
    // The manager lives on this thread - the one that called operator() - so
    // its event loop is the one `exec()` runs. Constructing it here means a
    // QtFetcher instance has no thread affinity of its own; only the call does.
    QNetworkAccessManager manager;

    QNetworkRequest request{ QUrl(QString::fromStdString(url)) };
    request.setAttribute(QNetworkRequest::RedirectPolicyAttribute,
                         QNetworkRequest::NoLessSafeRedirectPolicy);
    request.setTransferTimeout(kTransferTimeoutMs);

    QNetworkReply *reply = manager.get(request);
    if (reply == nullptr) {
        return Transfer::Failed;
    }

    // The store's sink returns false to stop the transfer (a write failure,
    // say); the fetcher must then stop and report false itself, so the store
    // drops the partial file.
    bool store_stopped = false;
    QObject::connect(reply, &QNetworkReply::readyRead, [&]() {
        if (store_stopped) {
            return;
        }
        const QByteArray chunk = reply->readAll();
        if (chunk.isEmpty()) {
            return;
        }
        if (!sink(as_bytes(chunk))) {
            store_stopped = true;
            reply->abort();
        }
    });

    QEventLoop loop;
    QObject::connect(reply, &QNetworkReply::finished, &loop, &QEventLoop::quit);

    // The watchdog that lets the controller's cancel reach a transfer that
    // streams for a long time. It aborts the reply, which emits finished and
    // unblocks the loop; the caller sees the abort as a Cancelled outcome.
    QTimer poll;
    if (cancel_ != nullptr) {
        QObject::connect(&poll, &QTimer::timeout, [&]() {
            if (cancel_->load(std::memory_order_relaxed)) {
                reply->abort();
            }
        });
        poll.start(kCancelPollMs);
    }

    loop.exec();

    // The final bytes can land with finished rather than a readyRead; drain
    // them so no tail is dropped.
    if (!store_stopped && reply->error() == QNetworkReply::NoError) {
        const QByteArray trailing = reply->readAll();
        if (!trailing.isEmpty() && !sink(as_bytes(trailing))) {
            store_stopped = true;
        }
    }

    Transfer outcome = Transfer::Failed;
    if (store_stopped) {
        outcome = Transfer::StoreStopped;
    } else if (cancel_ != nullptr && cancel_->load(std::memory_order_relaxed)) {
        outcome = Transfer::Cancelled;
    } else if (reply->error() == QNetworkReply::NoError) {
        outcome = Transfer::Complete;
    }

    reply->deleteLater();
    return outcome;
}
