// SPDX-License-Identifier: GPL-3.0-or-later
// SPDX-FileCopyrightText: 2026 Genesis-0 contributors
//
// Shared assertion tally: the `check(condition, what)` counter every suite's
// main() prints as "<n> checks, <n> failures" and returns as its exit code.
// The recent suites each carried their own copy of this; a single header keeps
// the counter and the summary line in one place. It is deliberately Qt-free -
// the app_log, cache_sweep and project_claim suites link genesis_host alone,
// so they cannot pull Wait.h's Qt includes just to count assertions.

#pragma once

#include <cmath>
#include <cstdint>
#include <cstdio>
#include <string>

namespace genesis::test {

inline int checks = 0;
inline int failures = 0;

inline void check(bool condition, const std::string &what)
{
    ++checks;
    if (!condition) {
        ++failures;
        std::printf("FAIL: %s\n", what.c_str());
    }
}

// Floating-point comparison with an explicit tolerance, reporting both values
// on failure so a near-miss is diagnosable from the log alone.
inline void check_near(double got, double want, double tolerance, const std::string &what)
{
    check(std::abs(got - want) <= tolerance,
          what + " (got " + std::to_string(got) + ", want " + std::to_string(want) + ")");
}

// Integer comparison reporting both values on failure.
inline void check_eq(std::int64_t got, std::int64_t want, const std::string &what)
{
    check(got == want,
          what + " (got " + std::to_string(got) + ", want " + std::to_string(want) + ")");
}

// Prints the tally line and returns the process exit code, so main() ends with
// `return summary();` and the output line stays byte-identical.
inline int summary()
{
    std::printf("%d checks, %d failures\n", checks, failures);
    return failures == 0 ? 0 : 1;
}

} // namespace genesis::test
