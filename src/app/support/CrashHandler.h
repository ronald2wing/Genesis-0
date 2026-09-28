// SPDX-License-Identifier: GPL-3.0-or-later
// SPDX-FileCopyrightText: 2026 Genesis-0 contributors
//
// The crash hooks, split out of the log sink so the diagnostic path and the
// crash path stay apart. install_crash_handler() is called once by the
// composition root; set_crash_log_path() is the seam AppLog's set_log_file
// drives so a crash report appends to the same run log every other line does.

#pragma once

namespace genesis::app {

// Installs the crash hooks: a std::set_terminate handler, and on Linux/BSD/
// macOS SIGSEGV/SIGABRT handlers that append a reason and a backtrace to the
// run log and stderr. On other platforms only the terminate handler runs, and
// it writes the reason to stderr alone.
void install_crash_handler();

// Sets the run-log path a crash report appends to; nullptr detaches before the
// file is destroyed. The path is copied into a fixed buffer because the handler
// runs where std::string is not safe.
void set_crash_log_path(const char *path);

} // namespace genesis::app
