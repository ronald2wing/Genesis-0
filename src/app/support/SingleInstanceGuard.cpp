// SPDX-License-Identifier: GPL-3.0-or-later
// SPDX-FileCopyrightText: 2026 Genesis-0 contributors

#include "app/support/SingleInstanceGuard.h"

#include <QCryptographicHash>
#include <QLocalServer>
#include <QLocalSocket>
#include <QStandardPaths>

#include <QByteArray>
#include <QString>

namespace {

// The per-user key is a stable digest of the home directory: the socket name
// never drifts across launches, and two users sharing a machine (and the same
// abstract socket namespace) get distinct names.
QString home_key()
{
    const QString home = QStandardPaths::writableLocation(QStandardPaths::HomeLocation);
    const QByteArray digest = QCryptographicHash::hash(home.toUtf8(), QCryptographicHash::Sha256);
    return QString::fromLatin1(digest.toHex().left(16));
}

} // namespace

SingleInstanceGuard::SingleInstanceGuard(QObject *parent) : QObject(parent)
{
    name_ = default_name();
}

QString SingleInstanceGuard::default_name()
{
    return QStringLiteral("genesis-app-") + home_key();
}

void SingleInstanceGuard::set_name(const QString &name)
{
    name_ = name;
}

SingleInstanceGuard::Status SingleInstanceGuard::acquire(const QString &path)
{
    const QString name = name_;

    // Is another instance already listening? If so, hand it the path and let
    // this process exit.
    QLocalSocket probe;
    probe.connectToServer(name);
    if (probe.waitForConnected(100)) {
        probe.write(path.toUtf8());
        probe.flush();
        probe.waitForBytesWritten(100);
        probe.disconnectFromServer();
        return Status::Forwarded;
    }

    // No live server answered. Clear any stale socket file left by a crashed
    // instance, then bind.
    QLocalServer::removeServer(name);
    server_ = new QLocalServer(this);
    connect(server_, &QLocalServer::newConnection, this, &SingleInstanceGuard::onNewConnection);
    if (server_->listen(name)) {
        return Status::Primary;
    }

    // Could not bind (e.g. a socket in another user's home). Single-instance
    // protection is best-effort; run anyway.
    delete server_;
    server_ = nullptr;
    return Status::Standalone;
}

void SingleInstanceGuard::onNewConnection()
{
    while (QLocalSocket *socket = server_->nextPendingConnection()) {
        connect(socket, &QLocalSocket::readyRead, this, [this, socket]() {
            const QString path = QString::fromUtf8(socket->readAll());
            if (!path.isEmpty()) {
                emit pathReceived(path);
            }
        });
        connect(socket, &QLocalSocket::disconnected, socket, &QLocalSocket::deleteLater);
    }
}
