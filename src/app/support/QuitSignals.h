// SPDX-License-Identifier: GPL-3.0-or-later
// SPDX-FileCopyrightText: 2026 Genesis-0 contributors
//
// Clean-quit signal handling: SIGTERM/SIGINT/SIGHUP ask the Qt event loop to
// quit instead of killing the process outright. Without this the app ignores
// SIGTERM (Qt installs no handler), so a terminal or session manager that
// stops the job escalates to SIGKILL - the "Killed" a user sees when closing
// the terminal or logging out. The handler is async-signal-safe: it only writes
// one byte to a self-pipe, and a QSocketNotifier on the read end calls
// QCoreApplication::quit() from the event loop.

#pragma once

#include <QObject>

class QSocketNotifier;

namespace genesis::app {

// Owns the self-pipe and the notifier. Construct once, after the
// QCoreApplication exists, and keep it alive for the process's life. The
// handlers are process-global; a second instance would overwrite the first's
// pipe, so construct exactly one.
class QuitSignals : public QObject
{
    Q_OBJECT

public:
    explicit QuitSignals(QObject *parent = nullptr);
    ~QuitSignals() override;

    QuitSignals(const QuitSignals &) = delete;
    QuitSignals &operator=(const QuitSignals &) = delete;

private:
    int read_fd_ = -1;
    int write_fd_ = -1;
    QSocketNotifier *notifier_ = nullptr;
};

} // namespace genesis::app
