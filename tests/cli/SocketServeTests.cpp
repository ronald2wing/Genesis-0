// SPDX-License-Identifier: GPL-3.0-or-later
// SPDX-FileCopyrightText: 2026 Genesis-0 contributors

#include <cerrno>
#include <csignal>
#include <cstdio>
#include <cstring>
#include <filesystem>
#include <optional>
#include <string>
#include <system_error>

#include <fcntl.h>
#include <sys/socket.h>
#include <sys/un.h>
#include <sys/wait.h>
#include <unistd.h>

#include <nlohmann/json.hpp>

#include "api/Api.h"
#include "cli/JsonRpc.h"
#include "cli/SocketServe.h"
#include "project/Editor.h"
#include "../support/Checks.h"

namespace {

using genesis::test::check;

namespace cli = genesis::cli;
namespace api = genesis::api;
namespace project = genesis::project;

// Writes `line` plus a newline to `fd`, retrying on partial writes.
bool client_write_line(int fd, const std::string &line)
{
    std::string data = line + "\n";
    std::size_t off = 0;
    while (off < data.size()) {
        const ssize_t n = ::write(fd, data.data() + off, data.size() - off);
        if (n > 0) {
            off += static_cast<std::size_t>(n);
            continue;
        }
        if (n < 0 && errno == EINTR) {
            continue;
        }
        return false;
    }
    return true;
}

// Reads one newline-terminated line from `fd` (terminator dropped), or nullopt
// at EOF/error. Byte-at-a-time is fine for the small test payloads.
std::optional<std::string> client_read_line(int fd)
{
    std::string line;
    char c;
    for (;;) {
        const ssize_t n = ::read(fd, &c, 1);
        if (n == 1) {
            if (c == '\n') {
                return line;
            }
            line.push_back(c);
            continue;
        }
        if (n == 0) {
            return line.empty() ? std::nullopt : std::optional<std::string>(std::move(line));
        }
        if (errno == EINTR) {
            continue;
        }
        return std::nullopt;
    }
}

// A fresh, per-process socket path in the temp dir.
std::string test_socket_path()
{
    return (std::filesystem::temp_directory_path()
            / ("genesis-serve-test." + std::to_string(::getpid()) + ".sock"))
            .string();
}

// Connects to the Unix socket at `path`, retrying while the child binds, and
// returns the connected fd or -1 when the server never came up.
int connect_client(const std::string &path)
{
    for (int attempt = 0; attempt < 200; ++attempt) {
        const int client = ::socket(AF_UNIX, SOCK_STREAM, 0);
        sockaddr_un addr{ };
        addr.sun_family = AF_UNIX;
        std::strncpy(addr.sun_path, path.c_str(), sizeof(addr.sun_path) - 1);
        if (::connect(client, reinterpret_cast<const sockaddr *>(&addr), sizeof(addr)) == 0) {
            return client;
        }
        ::close(client);
        ::usleep(10 * 1000);
    }
    return -1;
}

// Forks a `cmd_serve` child on `path` with stdout/stderr silenced, and returns
// the child pid (or -1 if fork failed). The child never returns.
pid_t spawn_server(const std::string &path)
{
    const pid_t pid = ::fork();
    if (pid != 0) {
        return pid;
    }
    const int devnull = ::open("/dev/null", O_WRONLY);
    if (devnull >= 0) {
        ::dup2(devnull, STDOUT_FILENO);
        ::dup2(devnull, STDERR_FILENO);
        ::close(devnull);
    }
    _exit(cli::cmd_serve(path));
}

// Counts the open file descriptors of `pid` via /proc/<pid>/fd, or 0 when the
// table is unreadable.
std::size_t count_open_fds(pid_t pid)
{
    const std::filesystem::path dir = "/proc/" + std::to_string(pid) + "/fd";
    std::error_code ec;
    std::size_t count = 0;
    for (std::filesystem::directory_iterator it(dir, ec);
         it != std::filesystem::directory_iterator(); it.increment(ec)) {
        if (ec) {
            break;
        }
        ++count;
    }
    return count;
}

void test_default_socket_path()
{
    const std::string path = cli::default_socket_path();
    check(path.find("genesis-cli.") != std::string::npos, "the default path names the program");
    check(path.size() >= 5 && path.substr(path.size() - 5) == ".sock",
          "the default path ends in .sock");
    check(path.find(std::to_string(::getpid())) != std::string::npos,
          "the default path embeds the pid");

    ::setenv("XDG_RUNTIME_DIR", "/tmp/genesis-xdg-run", 1);
    const std::string overridden = cli::default_socket_path();
    check(overridden.rfind("/tmp/genesis-xdg-run/", 0) == 0,
          "XDG_RUNTIME_DIR overrides the directory");
    ::unsetenv("XDG_RUNTIME_DIR");
}

// The shared codec, exercised in-process (no socket): the same function both
// transports route through.
void test_handle_line()
{
    project::Editor editor;
    const api::Services services;
    bool shutdown = false;

    const std::optional<std::string> reply = cli::handle_jsonrpc_line(
            editor, services, R"({"jsonrpc":"2.0","id":1,"method":"version"})", shutdown);
    check(reply.has_value(), "a version line gets a reply");
    if (reply) {
        const nlohmann::json parsed = nlohmann::json::parse(*reply);
        check(parsed["id"] == 1, "the reply echoes the id");
        check(parsed["result"]["apiVersion"] == "0.2", "the reply carries the contract version");
    }
    check(!shutdown, "version does not request shutdown");

    const std::optional<std::string> bad =
            cli::handle_jsonrpc_line(editor, services, "not json", shutdown);
    check(bad.has_value(), "a malformed line gets a reply");
    if (bad) {
        const nlohmann::json parsed = nlohmann::json::parse(*bad);
        check(parsed["error"]["code"] == -32700, "a malformed line is a parse error");
    }

    const std::optional<std::string> unknown = cli::handle_jsonrpc_line(
            editor, services, R"({"jsonrpc":"2.0","id":2,"method":"nope"})", shutdown);
    check(unknown.has_value(), "an unknown method gets a reply");
    if (unknown) {
        const nlohmann::json parsed = nlohmann::json::parse(*unknown);
        check(parsed["error"]["code"] == -32601, "an unknown method is method-not-found");
    }

    const std::optional<std::string> stop = cli::handle_jsonrpc_line(
            editor, services, R"({"jsonrpc":"2.0","id":3,"method":"shutdown"})", shutdown);
    check(stop.has_value() && shutdown, "shutdown flips the flag");
    if (stop) {
        const nlohmann::json parsed = nlohmann::json::parse(*stop);
        check(parsed["id"] == 3, "shutdown echoes the id");
        check(parsed.contains("result"), "shutdown returns a result");
    }

    const std::optional<std::string> blank =
            cli::handle_jsonrpc_line(editor, services, "   ", shutdown);
    check(!blank.has_value(), "a blank line is skipped");
}

// The real socket transport: fork a server, connect over a Unix domain socket,
// round-trip a request, survive malformed input, and shut down cleanly.
void test_serve_round_trip()
{
    const std::filesystem::path socket_path = std::filesystem::temp_directory_path()
            / ("genesis-serve-test." + std::to_string(::getpid()) + ".sock");
    ::unlink(socket_path.c_str());

    const pid_t pid = ::fork();
    check(pid >= 0, "fork succeeds");
    if (pid < 0) {
        return;
    }

    if (pid == 0) {
        // Child: serve quietly, so the banner does not pollute ctest output.
        const int devnull = ::open("/dev/null", O_WRONLY);
        if (devnull >= 0) {
            ::dup2(devnull, STDOUT_FILENO);
            ::dup2(devnull, STDERR_FILENO);
            ::close(devnull);
        }
        const int code = cli::cmd_serve(socket_path.string());
        _exit(code);
    }

    // Parent: retry connect while the child binds.
    int client = -1;
    for (int attempt = 0; attempt < 200; ++attempt) {
        client = ::socket(AF_UNIX, SOCK_STREAM, 0);
        sockaddr_un addr{ };
        addr.sun_family = AF_UNIX;
        std::strncpy(addr.sun_path, socket_path.c_str(), sizeof(addr.sun_path) - 1);
        if (::connect(client, reinterpret_cast<const sockaddr *>(&addr), sizeof(addr)) == 0) {
            break;
        }
        ::close(client);
        client = -1;
        ::usleep(10 * 1000);
    }
    check(client >= 0, "serve binds and accepts a client");

    if (client >= 0) {
        check(client_write_line(client, R"({"jsonrpc":"2.0","id":1,"method":"version"})"),
              "the client sends version");
        const std::optional<std::string> version = client_read_line(client);
        check(version.has_value(), "the client reads the version reply");
        if (version) {
            const nlohmann::json parsed = nlohmann::json::parse(*version);
            check(parsed["jsonrpc"] == "2.0", "the reply is JSON-RPC 2.0");
            check(parsed["id"] == 1, "the reply echoes the id");
            check(parsed["result"]["apiVersion"] == "0.2",
                  "the reply carries the contract version");
        }

        check(client_write_line(client, "not json"), "the client sends malformed input");
        const std::optional<std::string> malformed = client_read_line(client);
        check(malformed.has_value(), "the client reads the parse-error reply");
        if (malformed) {
            const nlohmann::json parsed = nlohmann::json::parse(*malformed);
            check(parsed["error"]["code"] == -32700, "malformed input is a parse error");
        }

        check(client_write_line(client, R"({"jsonrpc":"2.0","id":2,"method":"version"})"),
              "the client sends a second request");
        const std::optional<std::string> second = client_read_line(client);
        check(second.has_value(), "the server survives malformed input");
        if (second) {
            const nlohmann::json parsed = nlohmann::json::parse(*second);
            check(parsed["id"] == 2 && parsed["result"]["apiVersion"] == "0.2",
                  "the second reply is a normal result");
        }

        check(client_write_line(client, R"({"jsonrpc":"2.0","id":3,"method":"shutdown"})"),
              "the client sends shutdown");
        const std::optional<std::string> stop = client_read_line(client);
        check(stop.has_value(), "the client reads the shutdown reply");
        if (stop) {
            const nlohmann::json parsed = nlohmann::json::parse(*stop);
            check(parsed["id"] == 3 && parsed.contains("result"), "shutdown is acknowledged");
        }
        ::close(client);
    } else {
        // The child is still blocked in accept; ask it to stop so waitpid
        // below does not hang.
        ::kill(pid, SIGTERM);
    }

    int status = 0;
    ::waitpid(pid, &status, 0);
    check(WIFEXITED(status) && WEXITSTATUS(status) == 0, "serve exits 0 after a shutdown request");
    check(!std::filesystem::exists(socket_path), "serve removes the socket on exit");
}

// The server's open-fd count must return to its baseline after N full
// connect/disconnect cycles: a leaked fd per connection would add N. The count
// is read from /proc/<pid>/fd, so it observes the child (where the accept loop
// owns the fds), not the test process.
void test_fd_stability()
{
    const std::string socket_path = test_socket_path();
    ::unlink(socket_path.c_str());

    const pid_t pid = spawn_server(socket_path);
    check(pid >= 0, "fork succeeds for the fd-stability check");
    if (pid < 0) {
        return;
    }

    // One warm-up connection so the listener and any fork-shared fds settle,
    // then let the server finish closing the accepted fd before the baseline.
    int probe = connect_client(socket_path);
    check(probe >= 0, "serve accepts the warm-up connection");
    if (probe < 0) {
        ::kill(pid, SIGTERM);
        int reap = 0;
        ::waitpid(pid, &reap, 0);
        return;
    }
    client_write_line(probe, R"({"jsonrpc":"2.0","id":1,"method":"version"})");
    client_read_line(probe);
    ::close(probe);
    ::usleep(50 * 1000);

    const std::size_t baseline = count_open_fds(pid);
    check(baseline > 0, "the server's fd table is readable via /proc");

    constexpr int kCycles = 100;
    for (int i = 0; i < kCycles; ++i) {
        const int client = connect_client(socket_path);
        check(client >= 0, "serve accepts a cycle connection");
        if (client < 0) {
            break;
        }
        client_write_line(client, R"({"jsonrpc":"2.0","id":1,"method":"version"})");
        client_read_line(client);
        ::close(client);
    }
    ::usleep(50 * 1000);

    const std::size_t after = count_open_fds(pid);
    check(after == baseline, "the server's fd count is stable over N disconnect cycles");

    // Reap cleanly.
    int stopper = connect_client(socket_path);
    check(stopper >= 0, "serve still accepts after the cycle loop");
    if (stopper >= 0) {
        client_write_line(stopper, R"({"jsonrpc":"2.0","id":2,"method":"shutdown"})");
        client_read_line(stopper);
        ::close(stopper);
    } else {
        ::kill(pid, SIGTERM);
    }
    int status = 0;
    ::waitpid(pid, &status, 0);
    check(WIFEXITED(status) && WEXITSTATUS(status) == 0,
          "serve exits 0 after the fd-stability check");
}

// A client that vanishes mid-line (bytes with no terminating newline, then a
// close) must not crash the server or leak a connection: the next client still
// gets served.
void test_mid_line_disconnect()
{
    const std::string socket_path = test_socket_path();
    ::unlink(socket_path.c_str());

    const pid_t pid = spawn_server(socket_path);
    check(pid >= 0, "fork succeeds for the mid-line check");
    if (pid < 0) {
        return;
    }

    int client = connect_client(socket_path);
    check(client >= 0, "serve accepts the mid-line client");
    if (client >= 0) {
        const std::string partial = R"({"jsonrpc":"2.0","id":1,"method":"versio)"; // no newline
        (void)!::write(client, partial.data(), partial.size());
        ::close(client); // gone before the line terminates
    }

    client = connect_client(socket_path);
    check(client >= 0, "serve survives a mid-line disconnect");
    if (client >= 0) {
        check(client_write_line(client, R"({"jsonrpc":"2.0","id":2,"method":"version"})"),
              "the next client sends version");
        const std::optional<std::string> reply = client_read_line(client);
        check(reply.has_value(), "the next client reads a reply");
        if (reply) {
            const nlohmann::json parsed = nlohmann::json::parse(*reply);
            check(parsed["id"] == 2 && parsed["result"]["apiVersion"] == "0.2",
                  "the reply is a normal result");
        }
        client_write_line(client, R"({"jsonrpc":"2.0","id":3,"method":"shutdown"})");
        client_read_line(client);
        ::close(client);
    } else {
        ::kill(pid, SIGTERM);
    }

    int status = 0;
    ::waitpid(pid, &status, 0);
    check(WIFEXITED(status) && WEXITSTATUS(status) == 0,
          "serve exits 0 after a mid-line disconnect");
}

// A second SIGTERM delivered during shutdown is a no-op: the server still exits
// cleanly (0) and removes its socket rather than crashing or leaving a stale
// listener.
void test_double_sigterm()
{
    const std::string socket_path = test_socket_path();
    ::unlink(socket_path.c_str());

    const pid_t pid = spawn_server(socket_path);
    check(pid >= 0, "fork succeeds for the double-SIGTERM check");
    if (pid < 0) {
        return;
    }

    const int client = connect_client(socket_path);
    check(client >= 0, "serve is up before the signals");
    if (client >= 0) {
        ::close(client);
    }

    ::kill(pid, SIGTERM);
    ::kill(pid, SIGTERM);

    int status = 0;
    ::waitpid(pid, &status, 0);
    check(WIFEXITED(status) && WEXITSTATUS(status) == 0, "serve exits 0 after a double SIGTERM");
    check(!std::filesystem::exists(socket_path), "serve removes the socket after a double SIGTERM");
}

// A served socket confines writes to the allowed roots (home + cwd, widened by
// --allow-root): a project.save under an allowed root succeeds, and one outside
// every root is refused before dispatch with the Refused code.
void test_serve_confines_writes()
{
    const std::filesystem::path allowed = std::filesystem::temp_directory_path()
            / ("genesis-serve-allowed." + std::to_string(::getpid()));
    std::filesystem::create_directories(allowed);
    const std::string socket_path = test_socket_path();
    ::unlink(socket_path.c_str());

    const pid_t pid = ::fork();
    check(pid >= 0, "fork succeeds for the confinement check");
    if (pid < 0) {
        std::filesystem::remove_all(allowed);
        return;
    }
    if (pid == 0) {
        const int devnull = ::open("/dev/null", O_WRONLY);
        if (devnull >= 0) {
            ::dup2(devnull, STDOUT_FILENO);
            ::dup2(devnull, STDERR_FILENO);
            ::close(devnull);
        }
        const int code = cli::cmd_serve(socket_path, { allowed.string() });
        _exit(code);
    }

    const int client = connect_client(socket_path);
    check(client >= 0, "serve with an --allow-root accepts a client");

    if (client >= 0) {
        check(client_write_line(
                      client,
                      R"({"jsonrpc":"2.0","id":1,"method":"project.save","params":{"path":")"
                              + (allowed / "out.json").string() + R"("}})"),
              "the client sends a save under the allowed root");
        const std::optional<std::string> allowed_reply = client_read_line(client);
        check(allowed_reply.has_value(), "the client reads the allowed-save reply");
        if (allowed_reply) {
            const nlohmann::json parsed = nlohmann::json::parse(*allowed_reply);
            check(!parsed.contains("error"), "a save under the allowed root is not refused");
        }

        const std::string outside =
                (std::filesystem::temp_directory_path()
                 / ("genesis-serve-outside." + std::to_string(::getpid()) + ".json"))
                        .string();
        check(client_write_line(
                      client,
                      R"({"jsonrpc":"2.0","id":2,"method":"project.save","params":{"path":")"
                              + outside + R"("}})"),
              "the client sends a save outside the roots");
        const std::optional<std::string> refused_reply = client_read_line(client);
        check(refused_reply.has_value(), "the client reads the refused-save reply");
        if (refused_reply) {
            const nlohmann::json parsed = nlohmann::json::parse(*refused_reply);
            check(parsed["error"]["code"] == -32003, "the outside save is a Refused error");
            check(parsed["error"]["message"].is_string()
                          && parsed["error"]["message"].get<std::string>().find(outside)
                                  != std::string::npos,
                  "the refusal names the outside path");
        }

        check(client_write_line(client, R"({"jsonrpc":"2.0","id":3,"method":"shutdown"})"),
              "the client sends shutdown");
        client_read_line(client);
        ::close(client);
    } else {
        ::kill(pid, SIGTERM);
    }

    int status = 0;
    ::waitpid(pid, &status, 0);
    check(WIFEXITED(status) && WEXITSTATUS(status) == 0,
          "serve exits 0 after the confinement check");
    std::filesystem::remove_all(allowed);
}

} // namespace

int main()
{
    test_default_socket_path();
    test_handle_line();
    test_serve_round_trip();
    test_fd_stability();
    test_mid_line_disconnect();
    test_double_sigterm();
    test_serve_confines_writes();

    return genesis::test::summary();
}
