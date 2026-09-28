// SPDX-License-Identifier: GPL-3.0-or-later
// SPDX-FileCopyrightText: 2026 Genesis-0 contributors
//
// Windows-only smoke check for run_subprocess: it must launch cmd, capture both
// pipes, report the exit status, and report a missing executable as not
// started. This is the compile-and-run gate for the Boost.Process branch; the
// POSIX branch is covered by the Linux suites.

#include <cstdio>
#include <string>

#include "extensions/Subprocess.h"

namespace {

bool contains(const std::string &text, const char *needle)
{
    return text.find(needle) != std::string::npos;
}

} // namespace

int main()
{
    using genesis::extensions::run_subprocess;

    const auto echo = run_subprocess({ "cmd", "/c", "echo hello" });
    if (!echo.started || !echo.exited || echo.exit_status != 0
        || !contains(echo.stdout_text, "hello")) {
        std::printf("stdout capture failed: started=%d exited=%d status=%d out=[%s]\n",
                    echo.started, echo.exited, echo.exit_status, echo.stdout_text.c_str());
        return 1;
    }

    const auto error = run_subprocess({ "cmd", "/c", "echo problem 1>&2" });
    if (!error.started || !error.exited || error.exit_status != 0
        || !contains(error.stderr_text, "problem")) {
        std::printf("stderr capture failed: started=%d exited=%d status=%d err=[%s]\n",
                    error.started, error.exited, error.exit_status, error.stderr_text.c_str());
        return 1;
    }

    const auto code = run_subprocess({ "cmd", "/c", "exit 7" });
    if (!code.started || !code.exited || code.exit_status != 7) {
        std::printf("exit status failed: started=%d exited=%d status=%d\n", code.started,
                    code.exited, code.exit_status);
        return 1;
    }

    const auto missing = run_subprocess({ "genesis-no-such-program" });
    if (missing.started) {
        std::printf("a missing executable must report started=false\n");
        return 1;
    }

    std::printf("windows subprocess checks passed\n");
    return 0;
}
