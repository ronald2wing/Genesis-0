// SPDX-License-Identifier: GPL-3.0-or-later
// SPDX-FileCopyrightText: 2026 Genesis-0 contributors

#include "cli/GrpcServe.h"

#include <grpcpp/grpcpp.h>
#include <grpcpp/security/server_credentials.h>

#include <array>
#include <atomic>
#include <cerrno>
#include <csignal>
#include <cstdint>
#include <cstdlib>
#include <cstring>
#include <fstream>
#include <iostream>
#include <mutex>
#include <optional>
#include <span>
#include <sstream>
#include <string>
#include <string_view>
#include <thread>
#include <utility>

#include <arpa/inet.h>
#include <netdb.h>
#include <netinet/in.h>
#include <sys/socket.h>
#include <unistd.h>

#include "genesis.grpc.pb.h"
#include "genesis.pb.h"

#include "ai/Sha256.h"
#include "api/Api.h"
#include "cli/Commands.h"
#include "cli/JsonRpc.h"
#include "project/Editor.h"

namespace genesis::cli {

namespace {

// Set by the signal handler; polled by the serve loop. `volatile sig_atomic_t`
// is the one type safe to touch from a handler.
volatile std::sig_atomic_t g_stop = 0;

void handle_signal(int)
{
    g_stop = 1;
}

// The RPC-message bound: an explicit receive limit so a hostile client cannot
// hand the JSON-RPC codec an unbounded buffer. The codec's own 1 MiB request
// guard sits below this, so the margin covers protobuf framing. The send limit
// is set to the same value (gRPC's own default) so a reply is never larger
// than the receive side is willing to accept.
constexpr int kMaxMessageSize = 4 * 1024 * 1024;

// A comparison whose timing does not depend on how much of the token matches:
// both sides are hashed to a fixed 32-byte digest and the digests are XORed
// byte-wise with no short-circuit, so nothing about the secret leaks through
// how long the check takes.
bool constant_time_equal(std::string_view a, std::string_view b)
{
    const auto digest = [](std::string_view text) {
        ai::Sha256 hasher;
        hasher.update(std::span<const std::uint8_t>(
                reinterpret_cast<const std::uint8_t *>(text.data()), text.size()));
        return hasher.digest();
    };
    const std::array<std::uint8_t, 32> left = digest(a);
    const std::array<std::uint8_t, 32> right = digest(b);
    std::uint8_t diff = 0;
    for (std::size_t i = 0; i < left.size(); ++i) {
        diff = static_cast<std::uint8_t>(diff | (left[i] ^ right[i]));
    }
    return diff == 0;
}

// Reads a file's full contents, or nullopt with `why` set when it cannot be
// read (missing, unreadable, or an I/O error).
std::optional<std::string> read_file(const std::string &path, std::string &why)
{
    std::ifstream in(path, std::ios::binary);
    if (!in) {
        why = "cannot open " + path;
        return std::nullopt;
    }
    std::ostringstream buffer;
    buffer << in.rdbuf();
    if (in.bad()) {
        why = "cannot read " + path;
        return std::nullopt;
    }
    return buffer.str();
}

// Trims surrounding ASCII whitespace so a token file with a trailing newline
// (as editors write) still yields the exact token.
std::string trim_ascii_ws(std::string value)
{
    std::size_t begin = 0;
    while (begin < value.size()
           && (value[begin] == ' ' || value[begin] == '\t' || value[begin] == '\r'
               || value[begin] == '\n')) {
        ++begin;
    }
    std::size_t end = value.size();
    while (end > begin
           && (value[end - 1] == ' ' || value[end - 1] == '\t' || value[end - 1] == '\r'
               || value[end - 1] == '\n')) {
        --end;
    }
    return value.substr(begin, end - begin);
}

// The host portion of a "host:port" address, with an IPv6 literal's brackets
// stripped, so the loopback check sees the bare address.
std::string host_of(const std::string &addr)
{
    if (!addr.empty() && addr.front() == '[') {
        const std::size_t close = addr.find(']');
        return close == std::string::npos ? std::string() : addr.substr(1, close - 1);
    }
    const std::size_t colon = addr.rfind(':');
    return colon == std::string::npos ? addr : addr.substr(0, colon);
}

// Whether `host` resolves to loopback only (127.0.0.0/8 or ::1). A host that
// does not resolve, or that resolves to any non-loopback address, reads false -
// fail closed, because a name that names the world must not be served in the
// clear.
bool is_loopback_host(const std::string &host)
{
    if (host.empty()) {
        return false;
    }
    addrinfo hints{ };
    hints.ai_family = AF_UNSPEC;
    hints.ai_socktype = SOCK_STREAM;
    addrinfo *result = nullptr;
    if (getaddrinfo(host.c_str(), nullptr, &hints, &result) != 0) {
        return false;
    }
    bool all_loopback = true;
    for (const addrinfo *ai = result; ai != nullptr; ai = ai->ai_next) {
        if (ai->ai_family == AF_INET) {
            const auto *sa = reinterpret_cast<const sockaddr_in *>(ai->ai_addr);
            const std::uint32_t addr = ntohl(sa->sin_addr.s_addr);
            if ((addr & 0xff000000u) != 0x7f000000u) {
                all_loopback = false;
                break;
            }
        } else if (ai->ai_family == AF_INET6) {
            const auto *sa6 = reinterpret_cast<const sockaddr_in6 *>(ai->ai_addr);
            const auto &bytes = sa6->sin6_addr.s6_addr;
            bool is_v6_loopback = true;
            for (std::size_t i = 0; i + 1 < 16; ++i) {
                if (bytes[i] != 0) {
                    is_v6_loopback = false;
                    break;
                }
            }
            if (is_v6_loopback && bytes[15] != 1) {
                is_v6_loopback = false;
            }
            if (!is_v6_loopback) {
                all_loopback = false;
                break;
            }
        } else {
            all_loopback = false;
            break;
        }
    }
    freeaddrinfo(result);
    return all_loopback;
}

// The gRPC service. It funnels each CallRequest's JSON string through the same
// JSON-RPC codec the stdio `api` and Unix-socket `serve` transports use, so
// every existing API method works over gRPC with no per-method protobuf
// surface to keep in sync. Auth is enforced here, at the RPC boundary: the
// `authorization: Bearer <token>` client metadata is checked before dispatch,
// and a missing or wrong token is refused with UNAUTHENTICATED. The one shared
// Editor is guarded by a mutex because the sync gRPC server runs each call on
// its own thread, whereas the stdio/socket transports are single-client.
class GenesisService final : public genesis::v1::Genesis::Service
{
public:
    GenesisService(std::string token, std::atomic<bool> *shutdown_requested)
        : token_(std::move(token)), shutdown_requested_(shutdown_requested)
    {
        // Assigned (not member-initialised) to keep the declaration order:
        // `services_` follows `editor_`, which the initialiser list skips.
        services_ = cli_services();
    }

    grpc::Status Call(grpc::ServerContext *context, const genesis::v1::CallRequest *request,
                      genesis::v1::CallReply *reply) override
    {
        if (!authorized(context)) {
            return grpc::Status(grpc::StatusCode::UNAUTHENTICATED,
                                "missing or invalid bearer token");
        }
        bool shutdown = false;
        std::optional<std::string> out;
        {
            std::lock_guard<std::mutex> lock(mutex_);
            out = handle_jsonrpc_line(editor_, services_, request->json(), shutdown);
        }
        if (out) {
            reply->set_json(*out);
        }
        if (shutdown) {
            // Signal the serve loop to stop, rather than calling Shutdown()
            // here: Shutdown() blocks until this very call completes, so a
            // self-call would deadlock.
            shutdown_requested_->store(true);
        }
        return grpc::Status::OK;
    }

private:
    bool authorized(grpc::ServerContext *context) const
    {
        constexpr std::string_view kPrefix = "Bearer ";
        const auto range = context->client_metadata().equal_range("authorization");
        for (auto it = range.first; it != range.second; ++it) {
            const std::string_view value(it->second.data(), it->second.size());
            if (value.size() >= kPrefix.size() && value.compare(0, kPrefix.size(), kPrefix) == 0
                && constant_time_equal(value.substr(kPrefix.size()), token_)) {
                return true;
            }
        }
        return false;
    }

    std::string token_;
    std::mutex mutex_;
    project::Editor editor_;
    api::Services services_;
    std::atomic<bool> *shutdown_requested_;
};

} // namespace

TokenResolution resolve_grpc_token(const std::string &token_arg, const std::string &token_file)
{
    TokenResolution resolved;
    if (const char *env = std::getenv("GENESIS_GRPC_TOKEN"); env != nullptr && env[0] != '\0') {
        resolved.token = env;
        return resolved;
    }
    if (!token_file.empty()) {
        std::string why;
        const std::optional<std::string> contents = read_file(token_file, why);
        if (!contents) {
            resolved.error = why;
            return resolved;
        }
        const std::string token = trim_ascii_ws(*contents);
        if (token.empty()) {
            resolved.error = "token file " + token_file + " is empty";
            return resolved;
        }
        resolved.token = token;
        return resolved;
    }
    if (!token_arg.empty()) {
        resolved.token = token_arg;
        resolved.warning = "warning: --token puts the secret in the process list (visible "
                           "in ps); prefer --token-file or GENESIS_GRPC_TOKEN";
    }
    return resolved;
}

int cmd_grpc_serve(const std::string &addr, const std::string &token,
                   const std::optional<std::string> &cert_path,
                   const std::optional<std::string> &key_path)
{
    if (token.empty()) {
        std::cerr << "error: --grpc requires a token; refusing to start "
                     "without one\n";
        return EXIT_FAIL;
    }

    // TLS is configured only when BOTH halves are present; one without the
    // other is a misconfiguration, not a silent downgrade to plaintext.
    const bool tls = cert_path.has_value() || key_path.has_value();
    if (cert_path.has_value() != key_path.has_value()) {
        std::cerr << "error: --grpc-cert and --grpc-key must be given together;"
                     " refusing to start\n";
        return EXIT_FAIL;
    }

    // A non-loopback bind exposes the powerful arbitrary-file read/write API
    // (project.open/project.save) to the network. Refuse it unless TLS is on;
    // the loopback default stays plaintext exactly as before.
    const std::string host = host_of(addr);
    if (!is_loopback_host(host) && !tls) {
        std::cerr << "error: refusing to bind " << addr
                  << ": a non-loopback gRPC bind requires TLS "
                     "(--grpc-cert/--grpc-key); use a loopback address for the "
                     "insecure default\n";
        return EXIT_FAIL;
    }

    std::shared_ptr<grpc::ServerCredentials> credentials = grpc::InsecureServerCredentials();
    if (tls) {
        std::string cert_error;
        std::string key_error;
        const std::optional<std::string> cert = read_file(*cert_path, cert_error);
        const std::optional<std::string> key = read_file(*key_path, key_error);
        if (!cert) {
            std::cerr << "error: " << cert_error << '\n';
            return EXIT_FAIL;
        }
        if (!key) {
            std::cerr << "error: " << key_error << '\n';
            return EXIT_FAIL;
        }
        grpc::SslServerCredentialsOptions tls_options;
        tls_options.pem_key_cert_pairs.push_back({ *key, *cert }); // private_key, cert_chain
        credentials = grpc::SslServerCredentials(tls_options);
    }

    // Stop cleanly on SIGINT/SIGTERM and ignore SIGPIPE, as `serve` does.
    struct sigaction action{ };
    action.sa_handler = handle_signal;
    sigemptyset(&action.sa_mask);
    if (sigaction(SIGINT, &action, nullptr) != 0 || sigaction(SIGTERM, &action, nullptr) != 0) {
        std::cerr << "error: could not install signal handlers: " << std::strerror(errno) << '\n';
        return EXIT_FAIL;
    }
    std::signal(SIGPIPE, SIG_IGN);

    std::atomic<bool> shutdown_requested{ false };
    GenesisService service(token, &shutdown_requested);

    grpc::ServerBuilder builder;
    builder.SetMaxReceiveMessageSize(kMaxMessageSize);
    builder.SetMaxSendMessageSize(kMaxMessageSize);
    int bound_port = 0;
    builder.AddListeningPort(addr, credentials, &bound_port);
    builder.RegisterService(&service);
    std::unique_ptr<grpc::Server> server = builder.BuildAndStart();
    if (server == nullptr) {
        std::cerr << "error: could not bind " << addr << '\n';
        return EXIT_FAIL;
    }

    // The host before the last ':' (an IPv6 literal like [::1] keeps its
    // brackets in the log); the port is whatever AddListeningPort actually
    // bound (so a port-0 request logs the real ephemeral port).
    const std::string listen_host = addr.substr(0, addr.rfind(':'));
    std::cout << "serving gRPC on " << listen_host << ':' << bound_port
              << (tls ? " (TLS," : " (insecure,") << " Bearer token required)\n"
              << std::flush;

    std::jthread waiter([&server] { server->Wait(); });
    while (g_stop == 0 && !shutdown_requested.load()) {
        sleep(1);
    }
    server->Shutdown();
    waiter.join();
    return EXIT_OK;
}

} // namespace genesis::cli
