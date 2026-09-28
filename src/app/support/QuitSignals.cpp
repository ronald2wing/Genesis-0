// SPDX-License-Identifier: GPL-3.0-or-later
// SPDX-FileCopyrightText: 2026 Genesis-0 contributors

#include "app/support/QuitSignals.h"

#include "app/support/AppLog.h"

#include <QCoreApplication>
#include <QSocketNotifier>

#include <csignal>
#include <fcntl.h>
#include <unistd.h>

namespace genesis::app {

namespace {

// The write end of the self-pipe, mirrored into a file-scope int so the signal
// handler (which takes no context) can reach it. -1 means "not installed".
volatile std::sig_atomic_t g_write_fd = -1;

// The last signal received, recorded by the handler and read by the notifier.
// sig_atomic_t is the only type a handler may safely write; the notifier logs
// it from the event loop, where app_log is safe to call.
volatile std::sig_atomic_t g_last_signal = 0;

// Async-signal-safe: write() is on the POSIX safe list. A full pipe drops the
// byte, which is fine - one pending byte is enough to wake the notifier.
extern "C" void quit_signal_handler(int signal_number)
{
    g_last_signal = signal_number;
    const int fd = g_write_fd;
    if (fd >= 0) {
        const char byte = 1;
        const ssize_t ignored = ::write(fd, &byte, 1);
        (void)ignored;
    }
}

const char *signal_name(int signal_number)
{
    switch (signal_number) {
    case SIGTERM:
        return "SIGTERM";
    case SIGINT:
        return "SIGINT";
    case SIGHUP:
        return "SIGHUP";
    default:
        return "signal";
    }
}

} // namespace

QuitSignals::QuitSignals(QObject *parent) : QObject(parent)
{
    int fds[2];
    if (::pipe(fds) != 0) {
        return;
    }
    read_fd_ = fds[0];
    write_fd_ = fds[1];

    // Non-blocking read end so the notifier never stalls the event loop.
    const int flags = ::fcntl(read_fd_, F_GETFL, 0);
    if (flags >= 0) {
        ::fcntl(read_fd_, F_SETFL, flags | O_NONBLOCK);
    }

    g_write_fd = write_fd_;

    // SIGTERM/SIGINT/SIGHUP all mean "stop cleanly". SIGKILL cannot be caught,
    // which is exactly why the app must not ignore these: a session that sends
    // SIGTERM and gets no response escalates to SIGKILL.
    std::signal(SIGTERM, &quit_signal_handler);
    std::signal(SIGINT, &quit_signal_handler);
    std::signal(SIGHUP, &quit_signal_handler);

    notifier_ = new QSocketNotifier(read_fd_, QSocketNotifier::Read, this);
    QObject::connect(notifier_, &QSocketNotifier::activated, this, [this]() {
        char buffer[16];
        const ssize_t ignored = ::read(read_fd_, buffer, sizeof buffer);
        (void)ignored;
        const int signal_number = g_last_signal;
        app_log("[app] received %s, quitting\n", signal_name(signal_number));
        QCoreApplication::quit();
    });
}

QuitSignals::~QuitSignals()
{
    // Detach the handler before the pipe closes, so a late signal cannot write
    // to a recycled descriptor.
    g_write_fd = -1;
    std::signal(SIGTERM, SIG_DFL);
    std::signal(SIGINT, SIG_DFL);
    std::signal(SIGHUP, SIG_DFL);
    if (read_fd_ >= 0) {
        ::close(read_fd_);
    }
    if (write_fd_ >= 0) {
        ::close(write_fd_);
    }
}

} // namespace genesis::app
