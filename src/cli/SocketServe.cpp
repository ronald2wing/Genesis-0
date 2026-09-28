// SPDX-License-Identifier: GPL-3.0-or-later
// SPDX-FileCopyrightText: 2026 Genesis-0 contributors

#include "cli/SocketServe.h"

#include <algorithm>
#include <cerrno>
#include <csignal>
#include <cstring>
#include <filesystem>
#include <iostream>
#include <optional>
#include <string>
#include <string_view>
#include <utility>

#include <sys/socket.h>
#include <sys/stat.h>
#include <sys/un.h>
#include <unistd.h>

#include <gst/gst.h>

#include "api/Api.h"
#include "api/PathPolicy.h"
#include "cli/Commands.h"
#include "cli/JsonRpc.h"
#include "project/Editor.h"

namespace genesis::cli {

namespace {

// Set by the signal handler; polled by the accept/serve loops. `volatile
// sig_atomic_t` is the one type safe to touch from a handler.
volatile std::sig_atomic_t g_stop = 0;

void handle_signal(int)
{
    g_stop = 1;
}

// Installs SIGINT/SIGTERM handlers (without SA_RESTART, so a blocked accept or
// read returns EINTR and the loop re-checks g_stop) and ignores SIGPIPE so a
// client that vanishes mid-reply cannot kill the server.
bool install_signal_handlers(std::string &error)
{
    struct sigaction action{ };
    action.sa_handler = handle_signal;
    sigemptyset(&action.sa_mask);
    action.sa_flags = 0;
    if (sigaction(SIGINT, &action, nullptr) != 0 || sigaction(SIGTERM, &action, nullptr) != 0) {
        error = std::string("could not install signal handlers: ") + std::strerror(errno);
        return false;
    }
    std::signal(SIGPIPE, SIG_IGN);
    return true;
}

// Owns a file descriptor and closes it exactly once on destruction, so every
// exit path - including one unwound by an exception - releases it rather than
// leaking one fd per accepted connection or a dangling listener.
class OwnedFd
{
public:
    OwnedFd() = default;
    explicit OwnedFd(int fd) : fd_(fd) { }
    OwnedFd(const OwnedFd &) = delete;
    OwnedFd &operator=(const OwnedFd &) = delete;
    OwnedFd(OwnedFd &&other) noexcept : fd_(other.release()) { }
    OwnedFd &operator=(OwnedFd &&other) noexcept
    {
        if (this != &other) {
            reset(other.release());
        }
        return *this;
    }
    ~OwnedFd() { reset(); }

    int get() const { return fd_; }
    explicit operator bool() const { return fd_ >= 0; }

    int release()
    {
        const int fd = fd_;
        fd_ = -1;
        return fd;
    }
    void reset(int fd = -1)
    {
        if (fd_ >= 0) {
            ::close(fd_);
        }
        fd_ = fd;
    }

private:
    int fd_ = -1;
};

// Binds and listens on a Unix domain socket at `path`. Returns the listening
// fd, or -1 with `error` set. A stale socket file from a previous run is
// removed first; a non-socket file at `path` is refused rather than unlinked.
int listen_on(const std::string &path, std::string &error)
{
    sockaddr_un addr{ };
    if (path.size() >= sizeof(addr.sun_path)) {
        error = "socket path too long: " + path;
        return -1;
    }

    struct stat st{ };
    if (::lstat(path.c_str(), &st) == 0) {
        if (!S_ISSOCK(st.st_mode)) {
            error = path + " exists and is not a socket";
            return -1;
        }
        ::unlink(path.c_str());
    }

    const int fd = ::socket(AF_UNIX, SOCK_STREAM, 0);
    if (fd < 0) {
        error = std::string("socket: ") + std::strerror(errno);
        return -1;
    }

    addr.sun_family = AF_UNIX;
    std::strncpy(addr.sun_path, path.c_str(), sizeof(addr.sun_path) - 1);
    if (::bind(fd, reinterpret_cast<const sockaddr *>(&addr), sizeof(addr)) != 0) {
        error = "bind " + path + ": " + std::strerror(errno);
        ::close(fd);
        return -1;
    }
    if (::listen(fd, 1) != 0) {
        error = "listen " + path + ": " + std::strerror(errno);
        ::close(fd);
        return -1;
    }
    return fd;
}

// One newline-delimited line read off the socket. `oversized` is set when the
// line exceeded kMaxJsonRequestBytes; its bytes are then discarded (through the
// newline) rather than returned, so a hostile client cannot make the reader
// buffer an unbounded line.
struct Line
{
    std::string text;
    bool oversized = false;
};

// Reads newline-delimited lines from a socket fd, buffering across reads. Each
// `next()` returns one line without its terminator (a trailing '\r' is
// dropped), or nullopt once the peer has closed and no bytes remain. A line
// longer than the request cap is returned oversized with an empty `text`, and
// the next line starts clean.
class LineReader
{
public:
    explicit LineReader(int fd) : fd_(fd) { }

    std::optional<Line> next()
    {
        for (;;) {
            const std::size_t nl = buffer_.find('\n');
            if (nl != std::string::npos) {
                Line line;
                line.oversized = nl > kMaxJsonRequestBytes;
                if (!line.oversized) {
                    line.text = buffer_.substr(0, nl);
                    strip_cr(line.text);
                }
                buffer_.erase(0, nl + 1);
                return line;
            }
            // No newline yet. Once the pending line already exceeds the cap,
            // stop buffering and discard through its newline so the buffer
            // never grows past the cap by more than one read chunk.
            if (buffer_.size() > kMaxJsonRequestBytes) {
                buffer_.clear();
                if (!discard_to_newline()) {
                    return std::nullopt;
                }
                return Line{ std::string{ }, true };
            }
            char chunk[4096];
            const ssize_t n = ::read(fd_, chunk, sizeof(chunk));
            if (n > 0) {
                buffer_.append(chunk, static_cast<std::size_t>(n));
                continue;
            }
            if (n < 0 && errno == EINTR) {
                if (g_stop) {
                    return std::nullopt;
                }
                continue;
            }
            // EOF: a final line without a trailing newline is still a line.
            if (buffer_.empty()) {
                return std::nullopt;
            }
            Line line;
            line.oversized = buffer_.size() > kMaxJsonRequestBytes;
            if (!line.oversized) {
                line.text = std::move(buffer_);
                strip_cr(line.text);
            }
            buffer_.clear();
            return line;
        }
    }

private:
    static void strip_cr(std::string &line)
    {
        if (!line.empty() && line.back() == '\r') {
            line.pop_back();
        }
    }

    // Reads and discards bytes until a newline is consumed, re-buffering any
    // bytes that follow it. Returns false at EOF/stop. At most one read chunk
    // of discard data is scanned at a time, so memory stays bounded.
    bool discard_to_newline()
    {
        for (;;) {
            char chunk[4096];
            const ssize_t n = ::read(fd_, chunk, sizeof(chunk));
            if (n > 0) {
                const char *begin = chunk;
                const char *end = chunk + n;
                const char *nl = std::find(begin, end, '\n');
                if (nl != end) {
                    buffer_.assign(nl + 1, end);
                    return true;
                }
                continue;
            }
            if (n < 0 && errno == EINTR) {
                if (g_stop) {
                    return false;
                }
                continue;
            }
            return false;
        }
    }

    int fd_;
    std::string buffer_;
};

// Writes `text` fully, retrying on partial writes and EINTR. Returns false on
// a write error (the peer is gone); SIGPIPE is ignored, so a closed peer
// yields EPIPE here instead of killing the process.
bool write_all(int fd, std::string_view text)
{
    while (!text.empty()) {
        const ssize_t n = ::write(fd, text.data(), text.size());
        if (n > 0) {
            text.remove_prefix(static_cast<std::size_t>(n));
            continue;
        }
        if (n < 0 && errno == EINTR) {
            continue;
        }
        return false;
    }
    return true;
}

} // namespace

std::string default_socket_path()
{
    std::string dir = "/tmp";
    if (const char *xdg = std::getenv("XDG_RUNTIME_DIR"); xdg != nullptr && *xdg != '\0') {
        dir = xdg;
    }
    return (std::filesystem::path(dir) / ("genesis-cli." + std::to_string(::getpid()) + ".sock"))
            .string();
}

int cmd_serve(const std::string &path, const std::vector<std::string> &allow_roots)
{
    // The probe seam reaches real GStreamer, so the library must be up before
    // the first request. gst_init is idempotent; the stdio `api` verb calls it
    // too, but `serve` is a separate entry point.
    gst_init(nullptr, nullptr);
    std::string error;
    if (!install_signal_handlers(error)) {
        std::cerr << "error: " << error << '\n';
        return EXIT_FAIL;
    }

    const OwnedFd listen = OwnedFd(listen_on(path, error));
    if (!listen) {
        std::cerr << "error: " << error << '\n';
        return EXIT_FAIL;
    }

    project::Editor editor;
    // The CLI links the engine adapter for `render`, so the probe and the
    // effect catalogue are wired for real, as the stdio `api` verb wires them.
    // The extension store is served through its Manager seam.
    const api::Services services = cli_services();
    // A served socket is a remote surface, so it confines writes (home + cwd,
    // widened by each --allow-root) unlike the in-process `api` verb.
    std::vector<std::filesystem::path> roots;
    roots.reserve(allow_roots.size());
    for (const std::string &root : allow_roots) {
        roots.push_back(std::filesystem::path(root));
    }
    const api::PathPolicy policy = api::server_path_policy(roots);

    std::cout << "serving on " << path << '\n' << std::flush;

    bool shutdown = false;
    while (!shutdown && !g_stop) {
        const OwnedFd client = OwnedFd(::accept(listen.get(), nullptr, nullptr));
        if (!client) {
            if (errno == EINTR) {
                continue; // re-check g_stop at the top of the loop
            }
            std::cerr << "error: accept: " << std::strerror(errno) << '\n';
            continue;
        }
        LineReader reader(client.get());
        while (!shutdown && !g_stop) {
            const std::optional<Line> line = reader.next();
            if (!line) {
                break;
            }
            if (line->oversized) {
                if (!write_all(client.get(), oversize_request_reply() + "\n")) {
                    break; // peer gone
                }
                continue;
            }
            if (const std::optional<std::string> reply =
                        handle_jsonrpc_line(editor, services, line->text, shutdown, &policy)) {
                if (!write_all(client.get(), *reply + "\n")) {
                    break; // peer gone
                }
            }
        }
    }

    // `listen`'s destructor closes the listener; the accepted `client` was
    // closed by its own destructor at the end of each loop iteration.
    ::unlink(path.c_str());
    return EXIT_OK;
}

} // namespace genesis::cli
