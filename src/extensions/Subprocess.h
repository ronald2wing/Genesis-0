// SPDX-License-Identifier: GPL-3.0-or-later
// SPDX-FileCopyrightText: 2026 Genesis-0 contributors

#pragma once

#include <string>
#include <vector>

namespace genesis::extensions {

// The outcome of running one child process: whether it could be started,
// whether it exited normally (rather than being killed by a signal), its exit
// status, and the captured stdout/stderr.
struct SubprocessResult
{
    bool started = false;
    bool exited = false;
    int exit_status = 0;
    std::string stdout_text;
    std::string stderr_text;
};

// Runs `argv` as an execvp-style argument vector (argv[0] is the program name;
// no trailing nullptr is required) with no shell, so no argument is ever
// re-parsed by one. Both stdout and stderr are captured; the child is reaped
// before this returns. `started` is false only when fork/pipe fails. An
// execvp failure (the program does not exist) reports `exited` true with
// `exit_status` 127, the conventional `_exit` code for a failed exec.
SubprocessResult run_subprocess(const std::vector<std::string> &argv);

} // namespace genesis::extensions
