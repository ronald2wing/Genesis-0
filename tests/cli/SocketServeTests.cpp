// SPDX-License-Identifier: GPL-3.0-or-later
// SPDX-FileCopyrightText: 2026 Genesis-0 contributors

#include <cerrno>
#include <csignal>
#include <cstdio>
#include <cstring>
#include <filesystem>
#include <optional>
#include <string>

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

namespace {

int checks = 0;
int failures = 0;

void check(bool condition, const std::string &what)
{
    ++checks;
    if (!condition) {
        ++failures;
        std::printf("FAIL: %s\n", what.c_str());
    }
}

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

} // namespace

int main()
{
    test_default_socket_path();
    test_handle_line();
    test_serve_round_trip();

    std::printf("%d checks, %d failures\n", checks, failures);
    return failures == 0 ? 0 : 1;
}
