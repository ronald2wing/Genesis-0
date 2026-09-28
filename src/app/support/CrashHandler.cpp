// SPDX-License-Identifier: GPL-3.0-or-later
// SPDX-FileCopyrightText: 2026 Genesis-0 contributors

#include "app/support/CrashHandler.h"

#include <csignal>
#include <cstddef>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <exception>

// The crash hook's backtrace needs execinfo.h and POSIX write/open; both exist
// on Linux, the BSDs and macOS. Elsewhere only the terminate handler runs and
// it writes the reason to stderr alone.
#if defined(__GLIBC__) || defined(__APPLE__) || defined(__FreeBSD__) || defined(__OpenBSD__) \
        || defined(__NetBSD__)
#  include <execinfo.h>
#  include <fcntl.h>
#  include <unistd.h>
#  define GENESIS_HAS_BACKTRACE 1
#endif

namespace genesis::app {

namespace {

#if defined(GENESIS_HAS_BACKTRACE)
// The installed run log's path, mirrored into a fixed buffer so the crash
// handler - which runs where std::string is not safe - can reopen and append
// to it with open()/write().
char g_log_path[4096] = { '\0' };

// Set while a crash is being reported; a crash inside the handler (or the
// abort that follows a terminate) must not report twice and loop forever.
volatile std::sig_atomic_t g_crashing = 0;
#endif

#if defined(GENESIS_HAS_BACKTRACE)

// Writes exactly `len` bytes to `fd`, retrying short writes: a signal handler
// must not assume a full write, and losing the tail of a backtrace is worse
// than a second write().
void write_all(int fd, const char *bytes, std::size_t len)
{
    std::size_t sent = 0;
    while (sent < len) {
        const ssize_t n = ::write(fd, bytes + sent, len - sent);
        if (n <= 0) {
            return;
        }
        sent += static_cast<std::size_t>(n);
    }
}

void write_all(int fd, const char *text)
{
    write_all(fd, text, std::strlen(text));
}

// The one crash path: a reason line and a backtrace to stderr, and the same to
// the run log when one is installed. Async-signal-safe by construction - the
// only calls are write/open/close and backtrace/backtrace_symbols_fd, all on
// the safe list, and the single snprintf formats one int and two fixed strings,
// which glibc resolves without taking a lock. `g_crashing` stops a crash inside
// the handler (or the abort that follows a terminate) from looping.
void crash_report(const char *reason, int signal_number)
{
    if (g_crashing) {
        return;
    }
    g_crashing = 1;

    char header[128];
    int length = 0;
    if (signal_number > 0) {
        length = std::snprintf(header, sizeof header, "genesis crash: signal %d (%s)\n",
                               signal_number, reason);
    } else {
        length = std::snprintf(header, sizeof header, "genesis crash: %s\n", reason);
    }
    if (length < 0) {
        length = 0;
    } else if (length >= static_cast<int>(sizeof header)) {
        length = static_cast<int>(sizeof header) - 1;
    }

    write_all(STDERR_FILENO, header, static_cast<std::size_t>(length));
    write_all(STDERR_FILENO, "backtrace:\n");
    void *frames[64];
    const int count = ::backtrace(frames, 64);
    ::backtrace_symbols_fd(frames, count, STDERR_FILENO);

    if (g_log_path[0] != '\0') {
        const int fd = ::open(g_log_path, O_WRONLY | O_APPEND | O_CREAT, 0644);
        if (fd >= 0) {
            write_all(fd, header, static_cast<std::size_t>(length));
            write_all(fd, "backtrace:\n");
            ::backtrace_symbols_fd(frames, count, fd);
            ::close(fd);
        }
    }

    g_crashing = 0;
}

[[noreturn]] void terminate_handler()
{
    crash_report("terminate", 0);
    // Hand the abnormal exit back to the runtime so a core dump is produced as
    // usual, without re-entering this handler through the SIGABRT abort()
    // raises.
    std::signal(SIGABRT, SIG_DFL);
    std::abort();
}

void signal_handler(int signal_number)
{
    const char *name = signal_number == SIGSEGV ? "SIGSEGV"
            : signal_number == SIGABRT          ? "SIGABRT"
                                                : "SIGUNKNOWN";
    crash_report(name, signal_number);
    std::signal(signal_number, SIG_DFL);
    std::raise(signal_number);
}

#else // no backtrace: stderr-only terminate handler, no signal hooks

[[noreturn]] void terminate_handler()
{
    std::fprintf(stderr, "genesis crash: terminate\n");
    std::abort();
}

#endif

} // namespace

void install_crash_handler()
{
    std::set_terminate(&terminate_handler);
#if defined(GENESIS_HAS_BACKTRACE)
    std::signal(SIGSEGV, &signal_handler);
    std::signal(SIGABRT, &signal_handler);
#endif
}

void set_crash_log_path(const char *path)
{
#if defined(GENESIS_HAS_BACKTRACE)
    if (path != nullptr) {
        (void)std::snprintf(g_log_path, sizeof g_log_path, "%s", path);
    } else {
        g_log_path[0] = '\0';
    }
#else
    (void)path;
#endif
}

} // namespace genesis::app
