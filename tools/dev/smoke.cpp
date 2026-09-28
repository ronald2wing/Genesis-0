// SPDX-License-Identifier: GPL-3.0-or-later
// SPDX-FileCopyrightText: 2026 Genesis-0 contributors
//
// The smoke gate: spawn the app under the offscreen/software-GL environment,
// open each surface via --screenshot-open, sample the child's RSS from
// /proc/<pid>/status, capture stderr, and classify QML runtime messages. This
// is the standardized runtime gate the agents kept hand-rolling.

#include "tools/dev/commands.h"

#include <fcntl.h>
#include <poll.h>
#include <signal.h>
#include <sys/types.h>
#include <sys/wait.h>
#include <unistd.h>

#include <algorithm>
#include <cerrno>
#include <chrono>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <string>
#include <string_view>

#include "tools/dev/classify.h"
#include "tools/dev/json.h"

namespace genesis::dev {

namespace {

// The RSS snapshot read from /proc/<pid>/status.
struct Rss
{
    long long vmrss = 0;
    long long vmhwm = 0;
};

std::optional<Rss> read_rss(pid_t pid)
{
    std::ifstream in("/proc/" + std::to_string(pid) + "/status");
    if (!in) {
        return std::nullopt;
    }
    Rss rss;
    std::string line;
    while (std::getline(in, line)) {
        if (const auto v = parse_status_kib(line, "VmRSS")) {
            rss.vmrss = *v;
        } else if (const auto v = parse_status_kib(line, "VmHWM")) {
            rss.vmhwm = *v;
        }
    }
    return rss;
}

// The raw outcome of one app spawn: exit status, wall time, stderr, and the RSS
// samples taken while the child ran.
struct Watch
{
    bool spawned = false;
    bool timed_out = false;
    int exit_code = -1;
    long long wall_ms = 0;
    std::string stderr_text;
    long long peak_rss_kb = 0;
    std::vector<std::pair<long long, long long>> rss_trajectory;
};

// Forks `app` with the screenshot flags, redirects stdout to /dev/null, and
// reads stderr while polling /proc/<pid>/status every 200 ms until the child
// exits or the safety deadline (--exit-after + 30 s) passes.
Watch spawn_and_watch(const std::string &app, const std::string &png_path,
                      const std::string &surface, long ms)
{
    Watch watch;

    int pipefd[2];
    if (::pipe(pipefd) != 0) {
        return watch;
    }

    const pid_t pid = ::fork();
    if (pid < 0) {
        ::close(pipefd[0]);
        ::close(pipefd[1]);
        return watch;
    }

    if (pid == 0) {
        ::dup2(pipefd[1], STDERR_FILENO);
        ::close(pipefd[0]);
        ::close(pipefd[1]);
        const int devnull = ::open("/dev/null", O_WRONLY);
        if (devnull >= 0) {
            ::dup2(devnull, STDOUT_FILENO);
            ::close(devnull);
        }
        ::setenv("QT_QPA_PLATFORM", "offscreen", 1);
        ::setenv("LIBGL_ALWAYS_SOFTWARE", "1", 1);

        std::vector<std::string> args = { app, "--screenshot", png_path };
        if (surface != "shell") {
            args.push_back("--screenshot-open");
            args.push_back(surface);
        }
        args.push_back("--exit-after");
        args.push_back(std::to_string(ms));

        std::vector<char *> argv;
        for (std::string &arg : args) {
            argv.push_back(arg.data());
        }
        argv.push_back(nullptr);

        ::execvp(app.c_str(), argv.data());
        std::fprintf(stderr, "genesis-dev: exec %s: %s\n", app.c_str(), std::strerror(errno));
        _exit(127);
    }

    ::close(pipefd[1]);
    const int flags = ::fcntl(pipefd[0], F_GETFL, 0);
    ::fcntl(pipefd[0], F_SETFL, flags | O_NONBLOCK);

    watch.spawned = true;

    const auto start = std::chrono::steady_clock::now();
    const long long safety_ms = ms + 30000;
    int status = 0;
    bool reaped = false;

    auto elapsed_ms = [&start]() {
        return std::chrono::duration_cast<std::chrono::milliseconds>(
                       std::chrono::steady_clock::now() - start)
                .count();
    };

    for (;;) {
        pollfd pfd{ pipefd[0], POLLIN, 0 };
        if (::poll(&pfd, 1, 200) > 0 && (pfd.revents & POLLIN)) {
            char buf[4096];
            ssize_t read_count;
            while ((read_count = ::read(pipefd[0], buf, sizeof buf)) > 0) {
                watch.stderr_text.append(buf, static_cast<std::size_t>(read_count));
            }
        }

        const long long elapsed = elapsed_ms();
        if (const auto rss = read_rss(pid)) {
            watch.peak_rss_kb = std::max(watch.peak_rss_kb, std::max(rss->vmrss, rss->vmhwm));
            watch.rss_trajectory.emplace_back(elapsed, rss->vmrss);
        }

        if (::waitpid(pid, &status, WNOHANG) == pid) {
            reaped = true;
            watch.wall_ms = elapsed_ms();
            break;
        }

        if (elapsed >= safety_ms) {
            watch.timed_out = true;
            ::kill(pid, SIGKILL);
            ::waitpid(pid, &status, 0);
            reaped = true;
            watch.wall_ms = elapsed_ms();
            break;
        }
    }

    char buf[4096];
    ssize_t read_count;
    while ((read_count = ::read(pipefd[0], buf, sizeof buf)) > 0) {
        watch.stderr_text.append(buf, static_cast<std::size_t>(read_count));
    }
    ::close(pipefd[0]);

    if (!reaped) {
        ::waitpid(pid, &status, 0);
    }

    watch.exit_code = WIFEXITED(status) ? WEXITSTATUS(status) : -1;
    return watch;
}

// Splits stderr into QML runtime messages (failures) and informational [app]
// lines.
void classify_stderr(const std::string &stderr_text, std::vector<std::string> &qml_lines,
                     int &app_log_lines)
{
    std::size_t start = 0;
    while (start <= stderr_text.size()) {
        const std::size_t newline = stderr_text.find('\n', start);
        std::string_view line(stderr_text.data() + start,
                              (newline == std::string::npos ? stderr_text.size() : newline)
                                      - start);
        if (!line.empty() && line.back() == '\r') {
            line.remove_suffix(1);
        }
        if (is_qml_message(line)) {
            qml_lines.emplace_back(line);
        } else if (is_app_log(line)) {
            ++app_log_lines;
        }
        if (newline == std::string::npos) {
            break;
        }
        start = newline + 1;
    }
}

bool surface_ok(const SurfaceResult &result, long long max_rss_mb)
{
    return result.spawned && !result.timed_out && result.exit_code == 0 && result.png
            && result.png_bytes > 0 && result.qml_lines.empty()
            && result.peak_rss_kb / 1024 <= max_rss_mb;
}

// The app's no-argument launch builds a demo project from a temp fixture
// (AppShell::setupProject). That fixture is a test artifact, not shipped, and
// the temp dir can be cleaned between runs - so the gate must not depend on it
// existing. Generate it on demand with the same gst-launch command the adapter
// tests use, into the app's first-choice path, only when neither candidate is
// present. Returns false when no fixture could be produced (gst-launch absent
// or failed), which the caller reports as a gate setup failure rather than a
// per-surface UI failure.
bool ensure_demo_fixture()
{
    const std::filesystem::path primary =
            std::filesystem::temp_directory_path() / "ges-spike" / "a.webm";
    const std::filesystem::path fallback =
            std::filesystem::temp_directory_path() / "opencode" / "clip.webm";
    std::error_code ec;
    if (std::filesystem::exists(primary, ec) || std::filesystem::exists(fallback, ec)) {
        return true;
    }

    std::filesystem::create_directories(primary.parent_path(), ec);
    // 30 frames of testsrc at 30 fps is exactly one second; VP8/WebM is the
    // encoder guaranteed present on the dev host and in CI.
    const std::string command = "gst-launch-1.0 -q videotestsrc num-buffers=30 ! videoconvert ! "
                                "vp8enc ! webmmux ! filesink location=\""
            + primary.string() + "\" >/dev/null 2>&1";
    if (std::system(command.c_str()) != 0) {
        return false;
    }
    return std::filesystem::exists(primary, ec) && std::filesystem::file_size(primary, ec) > 0;
}

void write_report_json(const SmokeReport &report)
{
    Json json;
    json.begin_object();
    json.key("app");
    json.string(report.args.app);
    json.key("out");
    json.string(report.args.out);
    json.key("seconds_ms");
    json.integer(report.args.seconds_ms);
    json.key("max_rss_mb");
    json.integer(report.args.max_rss_mb);
    json.key("fixture_ready");
    json.boolean(report.fixture_ready);
    json.key("passed");
    json.integer(report.passed);
    json.key("failed");
    json.integer(report.failed);
    json.key("surfaces");
    json.begin_array();
    for (const SurfaceResult &result : report.surfaces) {
        json.begin_object();
        json.key("name");
        json.string(result.surface);
        json.key("spawned");
        json.boolean(result.spawned);
        json.key("timed_out");
        json.boolean(result.timed_out);
        json.key("exit_code");
        json.integer(result.exit_code);
        json.key("wall_ms");
        json.integer(result.wall_ms);
        json.key("png");
        json.boolean(result.png);
        json.key("png_bytes");
        json.integer(result.png_bytes);
        json.key("peak_rss_kb");
        json.integer(result.peak_rss_kb);
        json.key("peak_rss_mb");
        json.integer(result.peak_rss_kb / 1024);
        json.key("qml_messages");
        json.integer(static_cast<long long>(result.qml_lines.size()));
        json.key("qml_lines");
        json.begin_array();
        for (const std::string &line : result.qml_lines) {
            json.string(line);
        }
        json.end_array();
        json.key("app_log_lines");
        json.integer(result.app_log_lines);
        json.key("rss_kb_trajectory");
        json.begin_array();
        for (const auto &[t_ms, rss_kb] : result.rss_trajectory) {
            json.begin_array();
            json.integer(t_ms);
            json.integer(rss_kb);
            json.end_array();
        }
        json.end_array();
        json.key("ok");
        json.boolean(surface_ok(result, report.args.max_rss_mb));
        json.end_object();
    }
    json.end_array();
    json.end_object();

    const std::filesystem::path report_path =
            std::filesystem::path(report.args.out) / "report.json";
    std::ofstream out(report_path);
    if (out) {
        out << json.take() << '\n';
    } else {
        std::fprintf(stderr, "genesis-dev: cannot write %s\n", report_path.string().c_str());
    }
}

void print_human(const SmokeReport &report)
{
    std::printf("smoke: %zu surfaces, %d passed, %d failed (app=%s, out=%s)\n",
                report.surfaces.size(), report.passed, report.failed, report.args.app.c_str(),
                report.args.out.c_str());
    if (!report.fixture_ready) {
        std::printf("smoke: demo media fixture unavailable - the app's no-arg launch cannot "
                    "build its demo project (is gst-launch-1.0 installed?)\n");
    }
    std::printf("%-12s %4s %8s %5s %10s %10s %4s\n", "surface", "exit", "wall_ms", "png", "bytes",
                "peak_rss", "qml");
    for (const SurfaceResult &result : report.surfaces) {
        std::printf("%-12s %4d %8lld %5s %10lld %9lldM %4zu\n", result.surface.c_str(),
                    result.exit_code, result.wall_ms, result.png ? "yes" : "no", result.png_bytes,
                    result.peak_rss_kb / 1024, result.qml_lines.size());
    }

    bool first = true;
    for (const SurfaceResult &result : report.surfaces) {
        if (result.qml_lines.empty() && surface_ok(result, report.args.max_rss_mb)) {
            continue;
        }
        if (first) {
            std::printf("\nproblems:\n");
            first = false;
        }
        std::printf("  %s:", result.surface.c_str());
        if (!result.spawned) {
            std::printf(" spawn failed");
        }
        if (result.timed_out) {
            std::printf(" timed out");
        }
        if (result.exit_code != 0) {
            std::printf(" exit=%d", result.exit_code);
        }
        if (!result.png) {
            std::printf(" no-png");
        }
        if (!result.qml_lines.empty()) {
            std::printf("\n");
            for (const std::string &line : result.qml_lines) {
                std::printf("    %s\n", line.c_str());
            }
        } else {
            std::printf("\n");
        }
    }
}

} // namespace

SmokeReport run_smoke(const SmokeArgs &args)
{
    SmokeReport report;
    report.args = args;

    const std::vector<std::string> surfaces =
            args.surfaces.empty() ? default_surfaces() : args.surfaces;

    std::error_code ec;
    std::filesystem::create_directories(args.out, ec);

    // The app's no-arg demo launch needs a media fixture; make sure one exists
    // so a cleaned temp dir cannot masquerade as a UI regression.
    report.fixture_ready = ensure_demo_fixture();

    for (const std::string &surface : surfaces) {
        const std::string png_path =
                (std::filesystem::path(args.out) / (surface + ".png")).string();

        Watch watch = spawn_and_watch(args.app, png_path, surface, args.seconds_ms);

        SurfaceResult result;
        result.surface = surface;
        result.spawned = watch.spawned;
        result.timed_out = watch.timed_out;
        result.exit_code = watch.exit_code;
        result.wall_ms = watch.wall_ms;
        result.peak_rss_kb = watch.peak_rss_kb;
        result.rss_trajectory = std::move(watch.rss_trajectory);
        classify_stderr(watch.stderr_text, result.qml_lines, result.app_log_lines);

        result.png = std::filesystem::exists(png_path, ec);
        result.png_bytes = 0;
        if (result.png) {
            result.png_bytes = static_cast<long long>(std::filesystem::file_size(png_path, ec));
            if (ec) {
                result.png_bytes = 0;
            }
        }

        if (surface_ok(result, args.max_rss_mb)) {
            ++report.passed;
        } else {
            ++report.failed;
        }
        report.surfaces.push_back(std::move(result));
    }

    return report;
}

int smoke_command(const SmokeArgs &args)
{
    const SmokeReport report = run_smoke(args);
    write_report_json(report);

    if (args.json) {
        std::ifstream in(std::filesystem::path(args.out) / "report.json");
        if (in) {
            std::printf("%s",
                        std::string(std::istreambuf_iterator<char>(in),
                                    std::istreambuf_iterator<char>())
                                .c_str());
        }
    } else {
        print_human(report);
    }

    if (!report.fixture_ready) {
        return 2; // gate setup failure, not a per-surface UI failure
    }
    return report.failed == 0 ? 0 : 1;
}

} // namespace genesis::dev
