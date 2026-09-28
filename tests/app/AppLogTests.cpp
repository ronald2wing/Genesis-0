// SPDX-License-Identifier: GPL-3.0-or-later
// SPDX-FileCopyrightText: 2026 Genesis-0 contributors

// The app log's hermetic suite: level parsing and filtering, the GENESIS_LOG
// environment override, and the terminate handler's crash marker (a forked
// child calls std::terminate and the parent asserts the run log carries the
// marker). No Qt, no GStreamer - AppLog.cpp and LogFile are the only code under
// test.

#include <csignal>
#include <cstdio>
#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <iterator>
#include <string>
#include <string_view>

#if defined(__unix__)
#  include <fcntl.h>
#  include <sys/wait.h>
#  include <unistd.h>
#endif

#include "../support/Checks.h"

#include "app/support/AppLog.h"
#include "app/support/CrashHandler.h"
#include "workspace/Logs.h"

namespace app = genesis::app;
namespace workspace = genesis::workspace;
using genesis::test::check;
using genesis::test::summary;

namespace {

std::filesystem::path scratch(std::string_view name)
{
    const std::filesystem::path dir =
            std::filesystem::temp_directory_path() / ("genesis-applog-" + std::string(name));
    std::error_code error;
    std::filesystem::remove_all(dir, error);
    std::filesystem::create_directories(dir, error);
    return dir;
}

std::string read_all(const std::filesystem::path &file)
{
    std::ifstream in(file);
    return std::string(std::istreambuf_iterator<char>(in), std::istreambuf_iterator<char>());
}

// The run log this scratch dir's LogFile opened (there is exactly one).
std::string read_run_log(const std::filesystem::path &dir)
{
    for (const auto &entry : std::filesystem::directory_iterator(dir)) {
        const std::string name = entry.path().filename().string();
        if (name.rfind("genesis-", 0) == 0 && name.ends_with(".log")) {
            return read_all(entry.path());
        }
    }
    return { };
}

void test_level_names_round_trip()
{
    check(app::parse_log_level("error") == app::LogLevel::Error, "error parses");
    check(app::parse_log_level("warn") == app::LogLevel::Warn, "warn parses");
    check(app::parse_log_level("info") == app::LogLevel::Info, "info parses");
    check(app::parse_log_level("debug") == app::LogLevel::Debug, "debug parses");
    check(app::parse_log_level(" DEBUG ") == app::LogLevel::Debug, "case and spaces are ignored");
    check(app::parse_log_level("Warn") == app::LogLevel::Warn, "mixed case parses");
    check(!app::parse_log_level("trace").has_value(), "an unknown name is refused");
    check(!app::parse_log_level("loud").has_value(), "a non-level is refused");
    check(!app::parse_log_level("").has_value(), "an empty name is refused");

    for (const auto level :
         { app::LogLevel::Error, app::LogLevel::Warn, app::LogLevel::Info, app::LogLevel::Debug }) {
        const char *name = app::log_level_name(level);
        check(app::parse_log_level(name) == level, "a level name reads back to its level");
    }
}

void test_apply_log_level_reports_an_invalid_name()
{
    check(app::apply_log_level("debug") == app::LogLevel::Debug, "a valid name applies");
    check(app::log_level() == app::LogLevel::Debug, "the applied level sticks");

    app::apply_log_level("loud");
    check(app::log_level() == app::LogLevel::Info, "an invalid name falls back to info");

    app::set_log_level(app::LogLevel::Info);
}

void test_info_level_gates_debug()
{
    const std::filesystem::path dir = scratch("gating");
    auto log = workspace::LogFile::open(dir);
    check(log.has_value(), "opens a log");
    if (!log) {
        return;
    }
    app::set_log_file(&*log);
    app::set_log_level(app::LogLevel::Info);

    app::app_log_at(app::LogLevel::Error, "error line\n");
    app::app_log_at(app::LogLevel::Warn, "warn line\n");
    app::app_log("info line\n"); // the back-compatible convenience
    app::app_log_at(app::LogLevel::Debug, "debug line\n");
    log->close();

    const std::string contents = read_run_log(dir);
    check(contents.find("[error] error line") != std::string::npos, "error is written and tagged");
    check(contents.find("[warn] warn line") != std::string::npos, "warn is written and tagged");
    check(contents.find("info line") != std::string::npos, "info is written untagged");
    check(contents.find("debug line") == std::string::npos, "debug is dropped at info");

    app::set_log_file(nullptr);
    app::set_log_level(app::LogLevel::Info);
}

void test_debug_level_writes_debug()
{
    const std::filesystem::path dir = scratch("debug");
    auto log = workspace::LogFile::open(dir);
    check(log.has_value(), "opens a log");
    if (!log) {
        return;
    }
    app::set_log_file(&*log);
    app::set_log_level(app::LogLevel::Debug);

    app::app_log_at(app::LogLevel::Debug, "debug line\n");
    log->close();

    const std::string contents = read_run_log(dir);
    check(contents.find("[debug] debug line") != std::string::npos, "debug is written at debug");

    app::set_log_file(nullptr);
    app::set_log_level(app::LogLevel::Info);
}

void test_error_level_drops_everything_else()
{
    const std::filesystem::path dir = scratch("error");
    auto log = workspace::LogFile::open(dir);
    check(log.has_value(), "opens a log");
    if (!log) {
        return;
    }
    app::set_log_file(&*log);
    app::set_log_level(app::LogLevel::Error);

    app::app_log_at(app::LogLevel::Error, "error line\n");
    app::app_log_at(app::LogLevel::Warn, "warn line\n");
    app::app_log("info line\n");
    log->close();

    const std::string contents = read_run_log(dir);
    check(contents.find("[error] error line") != std::string::npos, "error is written at error");
    check(contents.find("warn line") == std::string::npos, "warn is dropped at error");
    check(contents.find("info line") == std::string::npos, "info is dropped at error");

    app::set_log_file(nullptr);
    app::set_log_level(app::LogLevel::Info);
}

void test_the_environment_overrides_the_level()
{
#if defined(__unix__)
    ::setenv("GENESIS_LOG", "warn", 1);
    check(app::apply_environment_log_level() == app::LogLevel::Warn,
          "GENESIS_LOG=warn applies the warn level");
    check(app::log_level() == app::LogLevel::Warn, "the environment level sticks");
    ::unsetenv("GENESIS_LOG");
    app::set_log_level(app::LogLevel::Info);
#else
    std::printf("SKIP: environment override needs setenv (non-Unix)\n");
#endif
}

void test_the_terminate_handler_writes_the_crash_marker()
{
#if defined(__unix__)
    const std::filesystem::path dir = scratch("crash");

    const pid_t pid = ::fork();
    check(pid >= 0, "fork succeeds");
    if (pid < 0) {
        return;
    }
    if (pid == 0) {
        // The child's stderr is silenced so the backtrace does not pollute the
        // ctest output; the marker is asserted through the run log below.
        const int devnull = ::open("/dev/null", O_WRONLY);
        if (devnull >= 0) {
            ::dup2(devnull, STDERR_FILENO);
            ::close(devnull);
        }
        auto log = workspace::LogFile::open(dir);
        if (log) {
            app::set_log_file(&*log);
        }
        app::install_crash_handler();
        app::app_log("child alive\n");
        std::terminate(); // never returns
    }

    int status = 0;
    ::waitpid(pid, &status, 0);
    check(WIFSIGNALED(status), "the child dies by signal, not a clean exit");

    const std::string contents = read_run_log(dir);
    check(contents.find("child alive") != std::string::npos, "the child's normal line is written");
    check(contents.find("genesis crash: terminate") != std::string::npos,
          "the terminate handler writes the crash marker");
#else
    std::printf("SKIP: the terminate-marker test needs fork (non-Unix)\n");
#endif
}

} // namespace

int main()
{
    test_level_names_round_trip();
    test_apply_log_level_reports_an_invalid_name();
    test_info_level_gates_debug();
    test_debug_level_writes_debug();
    test_error_level_drops_everything_else();
    test_the_environment_overrides_the_level();
    test_the_terminate_handler_writes_the_crash_marker();

    return summary();
}
