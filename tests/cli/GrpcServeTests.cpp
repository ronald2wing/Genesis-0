// SPDX-License-Identifier: GPL-3.0-or-later
// SPDX-FileCopyrightText: 2026 Genesis-0 contributors

#include <cerrno>
#include <chrono>
#include <csignal>
#include <cstdio>
#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <string>

#include <arpa/inet.h>
#include <fcntl.h>
#include <netinet/in.h>
#include <sys/socket.h>
#include <sys/wait.h>
#include <unistd.h>

#include <grpcpp/grpcpp.h>

#include <nlohmann/json.hpp>

#include "genesis.grpc.pb.h"
#include "genesis.pb.h"

#include "api/Api.h"
#include "cli/Commands.h"
#include "cli/GrpcServe.h"
#include "cli/JsonRpc.h"
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

// Reserves an ephemeral localhost TCP port and returns it, closing the socket
// so the child server can bind it. There is a small race window (another
// process could take the port), which the connect retry loop below absorbs.
int reserve_ephemeral_port()
{
    const int fd = ::socket(AF_INET, SOCK_STREAM, 0);
    if (fd < 0) {
        return -1;
    }
    sockaddr_in addr{ };
    addr.sin_family = AF_INET;
    addr.sin_addr.s_addr = htonl(INADDR_LOOPBACK);
    addr.sin_port = 0;
    int port = -1;
    if (::bind(fd, reinterpret_cast<const sockaddr *>(&addr), sizeof(addr)) == 0) {
        socklen_t len = sizeof(addr);
        if (::getsockname(fd, reinterpret_cast<sockaddr *>(&addr), &len) == 0) {
            port = ntohs(addr.sin_port);
        }
    }
    ::close(fd);
    return port;
}

// Reads one line (terminator dropped) from `fd`, or an empty string at EOF.
std::string read_line(int fd)
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
        if (n == 0 || (n < 0 && errno != EINTR)) {
            return line;
        }
    }
}

void test_grpc_round_trip()
{
    const std::string token = "test-secret-token";
    const int port = reserve_ephemeral_port();
    check(port > 0, "an ephemeral port is reserved");
    if (port <= 0) {
        return;
    }
    const std::string addr = "127.0.0.1:" + std::to_string(port);

    // The expected reply, from the shared codec (the stdio transport's path).
    project::Editor editor;
    const api::Services services;
    bool shutdown = false;
    const std::string version_line = R"({"jsonrpc":"2.0","id":1,"method":"version"})";
    const std::optional<std::string> codec_reply =
            cli::handle_jsonrpc_line(editor, services, version_line, shutdown);
    check(codec_reply.has_value(), "the stdio codec answers a version line");
    if (!codec_reply) {
        return;
    }

    // A pipe carries the child's bound-address log line back to the parent.
    int pipefd[2];
    const int pipe_ok = ::pipe(pipefd);
    check(pipe_ok == 0, "a pipe carries the child's log line");
    if (pipe_ok != 0) {
        return;
    }

    const pid_t pid = ::fork();
    check(pid >= 0, "fork succeeds");
    if (pid < 0) {
        return;
    }
    if (pid == 0) {
        ::close(pipefd[0]);
        const int devnull = ::open("/dev/null", O_WRONLY);
        if (devnull >= 0) {
            ::dup2(devnull, STDERR_FILENO);
            ::close(devnull);
        }
        ::dup2(pipefd[1], STDOUT_FILENO);
        ::close(pipefd[1]);
        const int code = cli::cmd_grpc_serve(addr, token);
        _exit(code);
    }

    ::close(pipefd[1]);
    const std::string log_line = read_line(pipefd[0]);
    ::close(pipefd[0]);
    check(log_line.find("serving gRPC on") != std::string::npos,
          "the server logs the bound address");
    if (log_line.find("serving gRPC on") == std::string::npos) {
        // The server failed to start; reap it and stop so the checks below do
        // not hang a client against a dead port.
        ::kill(pid, SIGTERM);
        int status = 0;
        ::waitpid(pid, &status, 0);
        return;
    }

    const auto channel = grpc::CreateChannel(addr, grpc::InsecureChannelCredentials());
    auto stub = genesis::v1::Genesis::NewStub(channel);
    const auto deadline = std::chrono::system_clock::now() + std::chrono::seconds(10);

    // 1. Authenticated version round-trip, byte-identical to the stdio codec.
    {
        grpc::ClientContext context;
        context.set_deadline(deadline);
        context.AddMetadata("authorization", "Bearer " + token);
        genesis::v1::CallRequest request;
        request.set_json(version_line);
        genesis::v1::CallReply reply;
        const grpc::Status status = stub->Call(&context, request, &reply);
        check(status.ok(), "an authenticated Call succeeds");
        check(reply.json() == *codec_reply, "the gRPC reply is the stdio codec's exact reply");
    }

    // 2. Missing token is refused before dispatch.
    {
        grpc::ClientContext context;
        context.set_deadline(deadline);
        genesis::v1::CallRequest request;
        request.set_json(version_line);
        genesis::v1::CallReply reply;
        const grpc::Status status = stub->Call(&context, request, &reply);
        check(status.error_code() == grpc::StatusCode::UNAUTHENTICATED,
              "a call without a token is UNAUTHENTICATED");
    }

    // 3. A wrong token is also refused.
    {
        grpc::ClientContext context;
        context.set_deadline(deadline);
        context.AddMetadata("authorization", "Bearer wrong-token");
        genesis::v1::CallRequest request;
        request.set_json(version_line);
        genesis::v1::CallReply reply;
        const grpc::Status status = stub->Call(&context, request, &reply);
        check(status.error_code() == grpc::StatusCode::UNAUTHENTICATED,
              "a call with the wrong token is UNAUTHENTICATED");
    }

    // 4. Malformed JSON maps to the JSON-RPC parse error, not a gRPC status.
    {
        grpc::ClientContext context;
        context.set_deadline(deadline);
        context.AddMetadata("authorization", "Bearer " + token);
        genesis::v1::CallRequest request;
        request.set_json("not json");
        genesis::v1::CallReply reply;
        const grpc::Status status = stub->Call(&context, request, &reply);
        check(status.ok(), "malformed JSON still returns an OK gRPC status");
        const nlohmann::json parsed = nlohmann::json::parse(reply.json());
        check(parsed["error"]["code"] == -32700, "malformed JSON maps to the JSON-RPC parse error");
    }

    // 5. shutdown ends the server.
    {
        grpc::ClientContext context;
        context.set_deadline(deadline);
        context.AddMetadata("authorization", "Bearer " + token);
        genesis::v1::CallRequest request;
        request.set_json(R"({"jsonrpc":"2.0","id":2,"method":"shutdown"})");
        genesis::v1::CallReply reply;
        const grpc::Status status = stub->Call(&context, request, &reply);
        check(status.ok(), "shutdown is acknowledged");
    }

    int status = 0;
    ::waitpid(pid, &status, 0);
    check(WIFEXITED(status) && WEXITSTATUS(status) == 0,
          "the server exits 0 after a shutdown request");
}

void test_non_loopback_without_tls_refused()
{
    // A wildcard or public bind exposes the arbitrary-file read/write API
    // (project.open/project.save) to the network; without TLS it must be
    // refused before anything is bound or any signal handler is installed.
    check(cli::cmd_grpc_serve("0.0.0.0:0", "token") == cli::EXIT_FAIL,
          "an IPv4 wildcard bind without TLS is refused");
    check(cli::cmd_grpc_serve("[::]:0", "token") == cli::EXIT_FAIL,
          "an IPv6 wildcard bind without TLS is refused");
    // The loopback default still starts (as today) with no certificate.
    // A real start blocks, so this is not exercised here; the round-trip test
    // above covers the loopback path end to end.
}

void test_json_structure_guards()
{
    project::Editor editor;
    const api::Services services;
    bool shutdown = false;

    // Nesting deeper than 64 levels is refused with the JSON-RPC parse error,
    // before the recursive parser can be driven into unbounded recursion.
    std::string deep;
    for (int i = 0; i < 100; ++i) {
        deep += '[';
    }
    for (int i = 0; i < 100; ++i) {
        deep += ']';
    }
    const std::optional<std::string> deep_reply =
            cli::handle_jsonrpc_line(editor, services, deep, shutdown);
    check(deep_reply.has_value(), "a too-deep request still produces a reply");
    const nlohmann::json deep_parsed = nlohmann::json::parse(*deep_reply);
    check(deep_parsed["error"]["code"] == -32700,
          "a too-deep request maps to the JSON-RPC parse error");

    // A request over 1 MiB is refused for the same reason.
    std::string big = "{\"m\":\"";
    big.append((1u << 20) + 1, 'a');
    big += "\"}";
    const std::optional<std::string> big_reply =
            cli::handle_jsonrpc_line(editor, services, big, shutdown);
    check(big_reply.has_value(), "an oversized request still produces a reply");
    const nlohmann::json big_parsed = nlohmann::json::parse(*big_reply);
    check(big_parsed["error"]["code"] == -32700,
          "an oversized request maps to the JSON-RPC parse error");
}

void test_token_resolution()
{
    // The environment variable wins over --token and is never warned about.
    ::setenv("GENESIS_GRPC_TOKEN", "env-secret", 1);
    const cli::TokenResolution from_env = cli::resolve_grpc_token("arg-secret", "");
    check(from_env.token && *from_env.token == "env-secret",
          "GENESIS_GRPC_TOKEN wins over --token");
    check(from_env.warning.empty(), "no ps warning when the env is used");
    check(from_env.error.empty(), "no error when the env is used");
    ::unsetenv("GENESIS_GRPC_TOKEN");

    // A token file wins over --token and has its trailing newline trimmed.
    const std::filesystem::path file =
            std::filesystem::temp_directory_path() / "genesis-grpc-token.txt";
    {
        std::ofstream out(file);
        out << "file-secret\n";
    }
    const cli::TokenResolution from_file = cli::resolve_grpc_token("arg-secret", file.string());
    check(from_file.token && *from_file.token == "file-secret",
          "--token-file wins over --token and trims the newline");
    check(from_file.warning.empty(), "no ps warning for --token-file");
    check(from_file.error.empty(), "no error for --token-file");
    std::filesystem::remove(file);

    // --token is the fallback and carries the ps-visibility warning.
    const cli::TokenResolution from_arg = cli::resolve_grpc_token("arg-secret", "");
    check(from_arg.token && *from_arg.token == "arg-secret",
          "--token is used when nothing else is set");
    check(!from_arg.warning.empty(), "--token warns it is visible in ps");
    check(from_arg.error.empty(), "--token is not an error");

    // No source, and an unreadable token file.
    check(!cli::resolve_grpc_token("", "").token, "no source yields no token");
    const cli::TokenResolution missing = cli::resolve_grpc_token("", "/nonexistent/genesis-token");
    check(!missing.token, "an unreadable token file yields no token");
    check(!missing.error.empty(), "an unreadable token file is reported");
}

} // namespace

int main()
{
    test_grpc_round_trip();
    test_non_loopback_without_tls_refused();
    test_json_structure_guards();
    test_token_resolution();

    std::printf("%d checks, %d failures\n", checks, failures);
    return failures == 0 ? 0 : 1;
}
