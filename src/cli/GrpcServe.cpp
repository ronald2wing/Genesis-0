// SPDX-License-Identifier: GPL-3.0-or-later
// SPDX-FileCopyrightText: 2026 Genesis-0 contributors

#include "cli/GrpcServe.h"

#include <grpcpp/grpcpp.h>
#include <grpcpp/security/server_credentials.h>

#include <algorithm>
#include <array>
#include <atomic>
#include <cerrno>
#include <chrono>
#include <condition_variable>
#include <csignal>
#include <cstdint>
#include <cstdlib>
#include <cstring>
#include <deque>
#include <fstream>
#include <iostream>
#include <memory>
#include <mutex>
#include <optional>
#include <span>
#include <sstream>
#include <string>
#include <string_view>
#include <thread>
#include <utility>
#include <vector>

#include <nlohmann/json.hpp>

#include <arpa/inet.h>
#include <netdb.h>
#include <netinet/in.h>
#include <sys/socket.h>
#include <unistd.h>

#include "genesis.grpc.pb.h"
#include "genesis.pb.h"

#include "ai/Sha256.h"
#include "api/Api.h"
#include "api/PathPolicy.h"
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

// How long an idle Events handler waits between polling for a queued event or
// a client disconnect. Long enough that an idle stream costs almost nothing;
// short enough that a vanished client is noticed promptly.
constexpr std::chrono::milliseconds kIdlePoll{ 500 };

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
            if ((addr & 0xff000000U) != 0x7f000000U) {
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

// A tiny in-process pub/sub bus for the Events stream. Each subscriber owns a
// bounded FIFO; a full queue drops its oldest event so one slow client can
// never stall the broadcaster (or the server). A subscriber may filter on one
// topic - an empty filter receives everything. Every method is thread-safe:
// the gRPC handler threads and the serve loop both touch the bus.
class EventBus
{
public:
    struct Subscriber
    {
        std::mutex mutex;
        std::condition_variable cv;
        std::deque<std::string> queue;
        std::string filter;
        std::atomic<bool> closed{ false };
    };

    // The per-subscriber inbox cap. A slow client that stops reading drops the
    // oldest notifications rather than growing an unbounded buffer.
    static constexpr std::size_t kQueueCapacity = 64;

    std::shared_ptr<Subscriber> subscribe(std::string filter)
    {
        auto subscriber = std::make_shared<Subscriber>();
        subscriber->filter = std::move(filter);
        std::lock_guard<std::mutex> lock(mutex_);
        subscribers_.push_back(subscriber);
        return subscriber;
    }

    void unsubscribe(const std::shared_ptr<Subscriber> &subscriber)
    {
        {
            std::lock_guard<std::mutex> lock(mutex_);
            subscriber->closed.store(true);
            subscribers_.erase(std::remove(subscribers_.begin(), subscribers_.end(), subscriber),
                               subscribers_.end());
        }
        drained_.notify_all();
        subscriber->cv.notify_all();
    }

    // Enqueues one notification on every subscriber that accepts its topic.
    // Never blocks: a slow subscriber drops its oldest event instead.
    void publish(const std::string &topic, std::string json)
    {
        std::lock_guard<std::mutex> lock(mutex_);
        for (const std::shared_ptr<Subscriber> &subscriber : subscribers_) {
            if (!subscriber->filter.empty() && subscriber->filter != topic) {
                continue;
            }
            std::lock_guard<std::mutex> sub_lock(subscriber->mutex);
            if (subscriber->closed.load()) {
                continue;
            }
            if (subscriber->queue.size() >= kQueueCapacity) {
                subscriber->queue.pop_front();
            }
            subscriber->queue.push_back(json);
            subscriber->cv.notify_all();
        }
    }

    // Marks every subscriber closed and wakes its handler, so each one drains
    // what is already queued and then returns.
    void close()
    {
        std::lock_guard<std::mutex> lock(mutex_);
        for (const std::shared_ptr<Subscriber> &subscriber : subscribers_) {
            std::lock_guard<std::mutex> sub_lock(subscriber->mutex);
            subscriber->closed.store(true);
            subscriber->cv.notify_all();
        }
    }

    // Blocks until every subscriber has unsubscribed (its handler returned),
    // or `timeout` elapses. The serve loop waits here after close() so the
    // final serve.shutdown notification is written out before gRPC cancels the
    // RPCs; the timeout bounds the wait against a client that is blocked in a
    // Write and cannot drain.
    void wait_drained(std::chrono::milliseconds timeout)
    {
        std::unique_lock<std::mutex> lock(mutex_);
        drained_.wait_for(lock, timeout, [this] { return subscribers_.empty(); });
    }

    // The next queued notification for `subscriber`, waiting up to `timeout`
    // for one to arrive. Returns nullopt when nothing arrived in time or the
    // subscriber was closed (the caller distinguishes the two via `closed`).
    std::optional<std::string> next_event(const std::shared_ptr<Subscriber> &subscriber,
                                          std::chrono::milliseconds timeout)
    {
        std::unique_lock<std::mutex> lock(subscriber->mutex);
        subscriber->cv.wait_for(lock, timeout, [&subscriber] {
            return !subscriber->queue.empty() || subscriber->closed.load();
        });
        if (subscriber->queue.empty()) {
            return std::nullopt;
        }
        std::string event = std::move(subscriber->queue.front());
        subscriber->queue.pop_front();
        return event;
    }

private:
    std::mutex mutex_;
    std::condition_variable drained_;
    std::vector<std::shared_ptr<Subscriber>> subscribers_;
};

// The set of methods that mutate project state, after which a `project.changed`
// notification is published. Reads (`version`, `project.get`, `project.document`,
// the catalogue/template/media/export/status reads) and the extension/config/AI
// management verbs are excluded.
bool is_project_mutating(std::string_view method)
{
    return method == "project.create" || method == "project.open" || method == "project.close"
            || method == "project.save" || method == "project.setVideo" || method == "edit.apply"
            || method == "edit.undo" || method == "edit.redo" || method == "media.import";
}

// The request's `method`, or empty when the line is not a readable request
// object. The codec already bounded the line, so re-parsing here is cheap.
std::string method_of(std::string_view request_json)
{
    try {
        const nlohmann::json parsed = nlohmann::json::parse(request_json);
        return parsed.is_object() ? parsed.value("method", "") : std::string{ };
    } catch (const nlohmann::json::exception &) {
        return { };
    }
}

// Whether a codec reply is a JSON-RPC result (success) rather than an error.
bool is_success_reply(std::string_view reply_json)
{
    try {
        const nlohmann::json parsed = nlohmann::json::parse(reply_json);
        return parsed.is_object() && !parsed.contains("error");
    } catch (const nlohmann::json::exception &) {
        return false;
    }
}

// The gRPC service. It funnels each CallRequest's JSON string through the same
// JSON-RPC codec the stdio `api` and Unix-socket `serve` transports use, so
// every existing API method works over gRPC with no per-method protobuf
// surface to keep in sync. Auth is enforced here, at the RPC boundary: the
// `authorization: Bearer <token>` client metadata is checked before dispatch,
// and a missing or wrong token is refused with UNAUTHENTICATED. The one shared
// Editor is guarded by a mutex because the sync gRPC server runs each call on
// its own thread, whereas the stdio/socket transports are single-client.
//
// A successful project-mutating Call publishes a `project.changed`
// notification on the shared bus, which the Events RPC fans out to its
// subscribers.
class GenesisService final : public genesis::v1::Genesis::Service
{
public:
    GenesisService(std::string token, api::PathPolicy policy, std::atomic<bool> *shutdown_requested,
                   std::shared_ptr<EventBus> bus)
        : token_(std::move(token)),
          policy_(std::move(policy)),
          shutdown_requested_(shutdown_requested),
          bus_(std::move(bus))
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
            out = handle_jsonrpc_line(editor_, services_, request->json(), shutdown, &policy_);
        }
        if (out) {
            reply->set_json(*out);
            if (is_success_reply(*out) && is_project_mutating(method_of(request->json()))) {
                bus_->publish(
                        "project.changed",
                        api::wire_notification("project.changed", nlohmann::json::object()).dump());
            }
        }
        if (shutdown) {
            // Signal the serve loop to stop, rather than calling Shutdown()
            // here: Shutdown() blocks until this very call completes, so a
            // self-call would deadlock.
            shutdown_requested_->store(true);
        }
        return grpc::Status::OK;
    }

    grpc::Status Events(grpc::ServerContext *context, const genesis::v1::EventsRequest *request,
                        grpc::ServerWriter<genesis::v1::Event> *writer) override
    {
        if (!authorized(context)) {
            return grpc::Status(grpc::StatusCode::UNAUTHENTICATED,
                                "missing or invalid bearer token");
        }
        const std::shared_ptr<EventBus::Subscriber> subscriber = bus_->subscribe(request->topic());
        for (;;) {
            const std::optional<std::string> event = bus_->next_event(subscriber, kIdlePoll);
            if (event) {
                genesis::v1::Event message;
                message.set_json(*event);
                if (!writer->Write(message)) {
                    break; // the client went away
                }
                continue;
            }
            if (subscriber->closed.load()) {
                break; // the server is shutting down; the queue is drained
            }
            if (context->IsCancelled()) {
                break; // the client disconnected while idle
            }
        }
        bus_->unsubscribe(subscriber);
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
    api::PathPolicy policy_;
    std::atomic<bool> *shutdown_requested_;
    std::shared_ptr<EventBus> bus_;
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
                   const std::optional<std::string> &key_path,
                   const std::vector<std::string> &allow_roots)
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
    const std::shared_ptr<EventBus> bus = std::make_shared<EventBus>();
    // A served gRPC endpoint is a remote surface, so it confines writes (home +
    // cwd, widened by each --allow-root) unlike the in-process `api` verb.
    std::vector<std::filesystem::path> roots;
    roots.reserve(allow_roots.size());
    for (const std::string &root : allow_roots) {
        roots.push_back(std::filesystem::path(root));
    }
    GenesisService service(token, api::server_path_policy(roots), &shutdown_requested, bus);

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
    // Broadcast the final event, then let subscribers drain it before gRPC
    // cancels the RPCs, so every connected client sees `serve.shutdown` and a
    // clean stream end rather than a mid-flight cancellation.
    bus->publish("serve.shutdown",
                 api::wire_notification("serve.shutdown", nlohmann::json::object()).dump());
    bus->close();
    bus->wait_drained(std::chrono::milliseconds(2000));
    server->Shutdown();
    waiter.join();
    return EXIT_OK;
}

} // namespace genesis::cli
