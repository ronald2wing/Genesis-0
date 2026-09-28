// SPDX-License-Identifier: GPL-3.0-or-later
// SPDX-FileCopyrightText: 2026 Genesis-0 contributors
//
// One-command agent verification: configure (if needed), build, run the full
// ctest suite, re-run each failed test in isolation to classify flake vs real
// failure, then fold in a smoke run.

#include "tools/dev/commands.h"

#include <sys/types.h>
#include <sys/wait.h>
#include <unistd.h>

#include <algorithm>
#include <cstdio>
#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <iterator>
#include <string>
#include <string_view>
#include <vector>

#include "tools/dev/json.h"

namespace genesis::dev {

namespace {

// A subprocess run: its exit status and combined stdout+stderr.
struct Capture
{
    int exit_code = -1;
    std::string output;
};

// Forks `args[0]` with stdout and stderr merged into a pipe and returns the
// exit status and the captured bytes once it (and its descendants) close the
// pipe.
Capture run_capture(const std::vector<std::string> &args)
{
    Capture capture;
    int pipefd[2];
    if (::pipe(pipefd) != 0) {
        return capture;
    }

    const pid_t pid = ::fork();
    if (pid < 0) {
        ::close(pipefd[0]);
        ::close(pipefd[1]);
        return capture;
    }
    if (pid == 0) {
        ::dup2(pipefd[1], STDOUT_FILENO);
        ::dup2(pipefd[1], STDERR_FILENO);
        ::close(pipefd[0]);
        ::close(pipefd[1]);

        std::vector<char *> argv;
        for (const std::string &arg : args) {
            argv.push_back(const_cast<char *>(arg.data()));
        }
        argv.push_back(nullptr);
        ::execvp(args[0].c_str(), argv.data());
        _exit(127);
    }

    ::close(pipefd[1]);
    char buf[4096];
    ssize_t read_count;
    while ((read_count = ::read(pipefd[0], buf, sizeof buf)) > 0) {
        capture.output.append(buf, static_cast<std::size_t>(read_count));
    }
    ::close(pipefd[0]);

    int status = 0;
    ::waitpid(pid, &status, 0);
    capture.exit_code = WIFEXITED(status) ? WEXITSTATUS(status) : -1;
    return capture;
}

// Returns the test names ctest listed after "The following tests FAILED:".
std::vector<std::string> parse_failed_tests(std::string_view output)
{
    const std::string_view marker = "The following tests FAILED:";
    const std::size_t at = output.find(marker);
    if (at == std::string_view::npos) {
        return { };
    }

    std::vector<std::string> failed;
    std::size_t line_start = at + marker.size();
    while (line_start < output.size()) {
        const std::size_t line_end = output.find('\n', line_start);
        std::string_view line =
                output.substr(line_start,
                              line_end == std::string_view::npos ? std::string_view::npos
                                                                 : line_end - line_start);
        // A failed-test line is `\t<number> - <name> (<reason>)`.
        const std::size_t dash = line.find(" - ");
        if (dash != std::string_view::npos && !line.empty()
            && (line.front() == ' ' || line.front() == '\t')) {
            std::string_view name = line.substr(dash + 3);
            const std::size_t space = name.find(' ');
            if (space != std::string_view::npos) {
                name = name.substr(0, space);
            }
            if (!name.empty()) {
                failed.emplace_back(name);
            }
        }
        if (line_end == std::string_view::npos) {
            break;
        }
        line_start = line_end + 1;
    }
    return failed;
}

// Counts the tests ctest -N would run (lines shaped `  Test #n: name`).
// ctest right-aligns the number, so a sub-100 test is `Test  #91:` (two
// spaces) and a 100+ test is `Test #100:`; match "Test", spaces, then '#'.
long long count_tests(const std::string &build_dir)
{
    const Capture capture = run_capture({ "ctest", "--test-dir", build_dir, "-N" });
    if (capture.exit_code != 0) {
        return -1;
    }
    long long count = 0;
    std::size_t start = 0;
    while (start < capture.output.size()) {
        const std::size_t newline = capture.output.find('\n', start);
        std::string_view line(capture.output.data() + start,
                              (newline == std::string::npos ? capture.output.size() : newline)
                                      - start);
        const std::size_t test = line.find("Test");
        if (test != std::string_view::npos) {
            const std::size_t digits = line.find_first_not_of(' ', test + 4);
            if (digits != std::string_view::npos && line[digits] == '#') {
                ++count;
            }
        }
        if (newline == std::string::npos) {
            break;
        }
        start = newline + 1;
    }
    return count;
}

// Prints the last `lines` lines of `output` to stderr (the tail of a failed
// command's log, so the user sees the error without the whole trace).
void print_tail(std::string_view output, int lines)
{
    std::vector<std::string_view> tail;
    std::size_t start = 0;
    while (start <= output.size()) {
        const std::size_t newline = output.find('\n', start);
        std::string_view line(output.data() + start,
                              (newline == std::string_view::npos ? output.size() : newline)
                                      - start);
        tail.push_back(line);
        if (newline == std::string_view::npos) {
            break;
        }
        start = newline + 1;
    }
    if (tail.size() > static_cast<std::size_t>(lines)) {
        tail.erase(tail.begin(), tail.end() - lines);
    }
    for (const std::string_view line : tail) {
        std::fprintf(stderr, "  %.*s\n", static_cast<int>(line.size()), line.data());
    }
}

} // namespace

int verify_command(const VerifyArgs &args)
{
    const std::string &build_dir = args.build_dir;

    bool configured = true;
    bool build_ok = false;
    bool ctest_ok = false;
    long long tests_total = -1;
    std::vector<std::string> flake;
    std::vector<std::string> real_failures;

    // Configure when the build dir has no cache yet.
    if (!std::filesystem::exists(std::filesystem::path(build_dir) / "CMakeCache.txt")) {
        std::fprintf(stderr, "[verify] configuring %s\n", build_dir.c_str());
        const Capture cfg = run_capture({ "cmake", "-S", ".", "-B", build_dir, "-G", "Ninja" });
        if (cfg.exit_code != 0) {
            configured = false;
            std::fprintf(stderr, "[verify] configure failed (exit %d)\n", cfg.exit_code);
            print_tail(cfg.output, 40);
        }
    }

    if (configured) {
        std::fprintf(stderr, "[verify] building %s\n", build_dir.c_str());
        const Capture build = run_capture({ "cmake", "--build", build_dir });
        build_ok = build.exit_code == 0;
        if (!build_ok) {
            std::fprintf(stderr, "[verify] build failed (exit %d)\n", build.exit_code);
            print_tail(build.output, 40);
        }
    }

    if (build_ok) {
        std::fprintf(stderr, "[verify] testing %s\n", build_dir.c_str());
        const Capture test =
                run_capture({ "ctest", "--test-dir", build_dir, "--output-on-failure" });
        ctest_ok = test.exit_code == 0;
        tests_total = count_tests(build_dir);
        if (!ctest_ok) {
            const std::vector<std::string> failed = parse_failed_tests(test.output);
            if (failed.empty()) {
                std::fprintf(stderr, "[verify] ctest failed (exit %d) but no test names parsed\n",
                             test.exit_code);
                print_tail(test.output, 40);
            }
            for (const std::string &name : failed) {
                std::fprintf(stderr, "[verify] re-running %s in isolation\n", name.c_str());
                const Capture isolate = run_capture({ "ctest", "--test-dir", build_dir, "-R",
                                                      "^" + name + "$", "--output-on-failure" });
                if (isolate.exit_code == 0) {
                    flake.push_back(name);
                } else {
                    real_failures.push_back(name);
                }
            }
        }
    }

    // Fold in the smoke gate (unless skipped or the build did not produce an app).
    bool smoke_ok = true;
    bool smoke_ran = false;
    SmokeReport smoke;
    if (args.skip_smoke) {
        std::fprintf(stderr, "[verify] smoke skipped\n");
    } else if (build_ok) {
        SmokeArgs smoke_args;
        smoke_args.app = (std::filesystem::path(build_dir) / "bin" / "genesis-app").string();
        smoke_args.out = (std::filesystem::path(build_dir) / "ai-smoke").string();
        std::fprintf(stderr, "[verify] smoke (%s)\n", smoke_args.app.c_str());
        smoke = run_smoke(smoke_args);
        smoke_ran = true;
        smoke_ok = smoke.failed == 0 && smoke.fixture_ready;
    }

    const bool any_flake = !flake.empty();
    const bool ok = configured && build_ok && ctest_ok && real_failures.empty() && smoke_ok
            && (!any_flake || !args.strict);

    if (args.json) {
        Json json;
        json.begin_object();
        json.key("build_dir");
        json.string(build_dir);
        json.key("configured");
        json.boolean(configured);
        json.key("build_ok");
        json.boolean(build_ok);
        json.key("tests_total");
        json.integer(tests_total);
        json.key("ctest_ok");
        json.boolean(ctest_ok);
        json.key("flaky");
        json.begin_array();
        for (const std::string &name : flake) {
            json.string(name);
        }
        json.end_array();
        json.key("real_failures");
        json.begin_array();
        for (const std::string &name : real_failures) {
            json.string(name);
        }
        json.end_array();
        json.key("smoke_ran");
        json.boolean(smoke_ran);
        json.key("smoke_passed");
        json.integer(smoke.passed);
        json.key("smoke_failed");
        json.integer(smoke.failed);
        json.key("ok");
        json.boolean(ok);
        json.end_object();
        std::printf("%s\n", json.take().c_str());
    } else {
        std::printf("verify: build=%s configure=%s build=%s tests=%lld/%lld", build_dir.c_str(),
                    configured ? "ok" : "FAILED", build_ok ? "ok" : "FAILED",
                    tests_total < 0 ? -1
                                    : tests_total - static_cast<long long>(flake.size())
                                    - static_cast<long long>(real_failures.size()),
                    tests_total);
        if (ctest_ok) {
            std::printf(" ctest=ok");
        } else {
            std::printf(" ctest=FAILED (real=%zu flaky=%zu)", real_failures.size(), flake.size());
        }
        if (smoke_ran) {
            std::printf(" smoke=%d/%d", smoke.passed, smoke.passed + smoke.failed);
        } else if (args.skip_smoke) {
            std::printf(" smoke=skipped");
        }
        std::printf("\n");
        for (const std::string &name : flake) {
            std::printf("  flaky (passes in isolation): %s\n", name.c_str());
        }
        for (const std::string &name : real_failures) {
            std::printf("  REAL FAILURE: %s\n", name.c_str());
        }
    }

    return ok ? 0 : 1;
}

} // namespace genesis::dev
