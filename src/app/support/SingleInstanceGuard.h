// SPDX-License-Identifier: GPL-3.0-or-later
// SPDX-FileCopyrightText: 2026 Genesis-0 contributors

#pragma once

#include <QObject>
#include <QString>

class QLocalServer;
class QLocalSocket;

// Ensures only one app instance runs per user. The first launch becomes the
// primary and listens on a per-user socket; a second launch connects to it,
// forwards its project path and exits 0, so opening a project in a running
// instance is a no-op + forward rather than a competing editor. When the
// socket cannot be bound (a stale or foreign lock), the instance runs
// Standalone: single-instance protection is best-effort and never blocks
// startup.
class SingleInstanceGuard : public QObject
{
    Q_OBJECT
public:
    enum class Status {
        Primary, // this instance now owns the name and receives forwards
        Forwarded, // a running instance accepted `path`; the caller must exit
        Standalone, // no primary and the name could not be bound; proceed alone
    };

    explicit SingleInstanceGuard(QObject *parent = nullptr);

    // The per-user socket name, stable across launches so a later instance
    // finds the earlier one and two users on one machine never collide.
    static QString default_name();

    // Overrides the socket name this guard binds and probes. Defaults to
    // default_name(); tests set a unique name so they never touch the
    // production per-user socket.
    void set_name(const QString &name);

    // Tries to become the primary. When another instance already holds the
    // name, forwards `path` to it and returns Forwarded. A bind failure returns
    // Standalone. Never blocks on a foreign/stale socket.
    Status acquire(const QString &path);

signals:
    // Emitted on the primary when a later launch forwarded a project path.
    void pathReceived(const QString &path);

private:
    void onNewConnection();

    QLocalServer *server_ = nullptr;
    QString name_;
};
