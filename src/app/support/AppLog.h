// SPDX-License-Identifier: GPL-3.0-or-later
// SPDX-FileCopyrightText: 2026 Genesis-0 contributors
//
// The app's one diagnostic sink: every message goes to stderr - the format the
// app already spoke - and, when a run log is installed, to that file too. The
// composition root opens the LogFile and installs it here; a null file (the
// open failed) leaves stderr as the only sink, so logging never breaks startup.

#pragma once

// The printf-format attribute is GCC/Clang-only; the strict warning set that
// would use it is applied only to those compilers (see genesis_warnings).
#if defined(__GNUC__) || defined(__clang__)
#  define GENESIS_PRINTF_FORMAT(fmt, args) __attribute__((format(printf, fmt, args)))
#else
#  define GENESIS_PRINTF_FORMAT(fmt, args)
#endif

namespace genesis::workspace {
class LogFile;
}

namespace genesis::app {

// Installs the run log `app_log` writes to, without taking ownership. Pass
// nullptr to detach before the file is destroyed. Single-threaded by contract:
// the app logs from the UI thread only.
void set_log_file(genesis::workspace::LogFile *log);

// Writes one printf-formatted message to stderr and, when a run log is
// installed, a timestamped copy to it.
void app_log(const char *format, ...) GENESIS_PRINTF_FORMAT(1, 2);

} // namespace genesis::app
