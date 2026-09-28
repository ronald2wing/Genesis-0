// SPDX-License-Identifier: GPL-3.0-or-later
// SPDX-FileCopyrightText: 2026 Genesis-0 contributors

#include <archive.h>
#include <archive_entry.h>

#include <arpa/inet.h>
#include <netinet/in.h>
#include <sys/socket.h>
#include <unistd.h>

#include <atomic>
#include <cstdint>
#include <filesystem>
#include <fstream>
#include <iterator>
#include <span>
#include <string>
#include <system_error>
#include <thread>
#include <utility>
#include <vector>

#include "ai/Sha256.h"
#include "core/Hex.h"
#include "extensions/SourceInstall.h"
#include "extensions/Sources.h"
#include "../support/Checks.h"

namespace {

using genesis::test::check;

namespace ext = genesis::extensions;

const std::filesystem::path kBase =
        std::filesystem::temp_directory_path() / "genesis-archive-source-tests";

std::string native_manifest(const std::string &id)
{
    return "[extension]\n"
           "id = \""
            + id
            + "\"\n"
              "name = \"Archive fixture\"\n"
              "version = 1\n"
              "api = 1\n"
              "entry = \"libfixture.so\"\n";
}

// Writes a plain tar with one regular file per pair. The fixture is generated
// rather than committed so the traversal case is explicit in the test source.
bool write_tar(const std::filesystem::path &path,
               const std::vector<std::pair<std::string, std::string>> &files)
{
    archive *writer = archive_write_new();
    if (writer == nullptr) {
        return false;
    }
    archive_write_set_format_pax_restricted(writer);
    if (archive_write_open_filename(writer, path.string().c_str()) != ARCHIVE_OK) {
        archive_write_free(writer);
        return false;
    }
    for (const auto &[name, body] : files) {
        const bool directory = name.ends_with('/');
        archive_entry *entry = archive_entry_new();
        archive_entry_set_pathname(entry, name.c_str());
        archive_entry_set_filetype(entry, directory ? AE_IFDIR : AE_IFREG);
        archive_entry_set_perm(entry, 0644);
        archive_entry_set_size(entry, directory ? 0 : static_cast<la_int64_t>(body.size()));
        bool written = archive_write_header(writer, entry) == ARCHIVE_OK;
        if (written && !directory) {
            written = archive_write_data(writer, body.data(), body.size()) >= 0;
        }
        archive_entry_free(entry);
        if (!written) {
            archive_write_free(writer);
            return false;
        }
    }
    archive_write_close(writer);
    archive_write_free(writer);
    return true;
}

// A one-request-at-a-time loopback HTTP server for the remote-archive path.
// The body is served with Content-Length; the test uses an ephemeral port so
// parallel test runs do not collide.
class LocalHttpServer
{
public:
    explicit LocalHttpServer(std::string body) : body_(std::move(body))
    {
        fd_ = ::socket(AF_INET, SOCK_STREAM, 0);
        if (fd_ < 0) {
            return;
        }
        sockaddr_in address{ };
        address.sin_family = AF_INET;
        address.sin_addr.s_addr = htonl(INADDR_LOOPBACK);
        address.sin_port = 0;
        if (::bind(fd_, reinterpret_cast<sockaddr *>(&address), sizeof(address)) != 0
            || ::listen(fd_, 8) != 0) {
            ::close(fd_);
            fd_ = -1;
            return;
        }
        socklen_t length = sizeof(address);
        if (::getsockname(fd_, reinterpret_cast<sockaddr *>(&address), &length) != 0) {
            ::close(fd_);
            fd_ = -1;
            return;
        }
        port_ = ntohs(address.sin_port);
        thread_ = std::thread([this] { serve(); });
    }

    ~LocalHttpServer()
    {
        stop_ = true;
        if (fd_ >= 0) {
            ::shutdown(fd_, SHUT_RDWR);
            ::close(fd_);
        }
        if (thread_.joinable()) {
            thread_.join();
        }
    }

    LocalHttpServer(const LocalHttpServer &) = delete;
    LocalHttpServer &operator=(const LocalHttpServer &) = delete;

    bool ok() const { return fd_ >= 0 && port_ != 0; }

    std::string url(std::string_view path) const
    {
        return "http://127.0.0.1:" + std::to_string(port_) + std::string(path);
    }

private:
    void serve()
    {
        while (!stop_) {
            const int client = ::accept(fd_, nullptr, nullptr);
            if (client < 0) {
                return;
            }
            char request[1024];
            (void)::read(client, request, sizeof(request));
            const std::string response = "HTTP/1.1 200 OK\r\nContent-Length: "
                    + std::to_string(body_.size()) + "\r\nConnection: close\r\n\r\n" + body_;
            std::size_t sent = 0;
            while (sent < response.size()) {
                const ssize_t written = ::send(client, response.data() + sent,
                                               response.size() - sent, MSG_NOSIGNAL);
                if (written <= 0) {
                    break;
                }
                sent += static_cast<std::size_t>(written);
            }
            ::close(client);
        }
    }

    int fd_ = -1;
    std::uint16_t port_ = 0;
    std::string body_;
    std::atomic<bool> stop_{ false };
    std::thread thread_;
};

std::string read_file(const std::filesystem::path &path)
{
    std::ifstream in(path, std::ios::binary);
    return std::string(std::istreambuf_iterator<char>(in), std::istreambuf_iterator<char>());
}

// The SHA-256 of the archive's raw bytes, the value a remote archive's catalog
// entry pins (the local path keeps the extracted-tree digest).
std::string file_digest(const std::filesystem::path &path)
{
    std::ifstream in(path, std::ios::binary);
    genesis::ai::Sha256 hash;
    char buffer[4096];
    while (in) {
        in.read(buffer, sizeof(buffer));
        const std::streamsize count = in.gcount();
        if (count > 0) {
            hash.update(
                    std::span<const std::uint8_t>(reinterpret_cast<const std::uint8_t *>(buffer),
                                                  static_cast<std::size_t>(count)));
        }
    }
    return genesis::core::hex_encode(hash.digest());
}

} // namespace

int main()
{
    std::error_code ec;
    std::filesystem::remove_all(kBase, ec);
    std::filesystem::create_directories(kBase);
    const std::filesystem::path store = kBase / "store";

    // A manifest at the archive root.
    const std::filesystem::path root_archive = kBase / "root.tar";
    check(write_tar(root_archive,
                    { { "extension.toml", native_manifest("archive.root") },
                      { "libfixture.so", "placeholder" } }),
          "the root archive fixture is written");
    const ext::InstallResult root =
            ext::install_from_source({ ext::SourceType::Archive, root_archive.string(), "", "" },
                                     store, ext::Origin::Developer);
    check(root.ok(),
          root.problems.empty() ? "root archive installs"
                                : "root archive installs: " + root.problems.front());
    if (root.installed) {
        check(std::filesystem::is_regular_file(*root.installed / "extension.toml"),
              "the root archive installed the manifest");
    }

    // A manifest under a subdir, the archive equivalent of a git subdir.
    const std::filesystem::path subdir_archive = kBase / "subdir.tar";
    check(write_tar(subdir_archive,
                    { { "pkg/", "" },
                      { "pkg/extension.toml", native_manifest("archive.subdir") },
                      { "pkg/libfixture.so", "placeholder" } }),
          "the subdir archive fixture is written");
    const ext::InstallResult subdir = ext::install_from_source(
            { ext::SourceType::Archive, subdir_archive.string(), "", "pkg" }, store,
            ext::Origin::Developer);
    check(subdir.ok(),
          subdir.problems.empty() ? "subdir archive installs"
                                  : "subdir archive installs: " + subdir.problems.front());

    // A file:// URL goes through the remote path: libcurl streams it to disk,
    // and the digest pins the archive bytes rather than the extracted tree.
    const std::filesystem::path remote_archive = kBase / "remote.tar";
    check(write_tar(remote_archive,
                    { { "extension.toml", native_manifest("archive.remote") },
                      { "libfixture.so", "placeholder" } }),
          "the remote archive fixture is written");
    const std::string remote_url = "file://" + remote_archive.string();
    const ext::InstallResult remote = ext::install_from_source(
            { ext::SourceType::Archive, remote_url, "", "" }, store, ext::Origin::Developer);
    check(remote.ok(),
          remote.problems.empty() ? "a file:// archive installs"
                                  : "a file:// archive installs: " + remote.problems.front());
    const std::string remote_digest = file_digest(remote_archive);
    const ext::InstallResult verified =
            ext::install_from_source({ ext::SourceType::Archive, remote_url, "", "" }, store,
                                     ext::Origin::Developer, remote_digest);
    check(verified.ok(),
          verified.problems.empty()
                  ? "the remote archive digest verifies"
                  : "the remote archive digest verifies: " + verified.problems.front());
    const ext::InstallResult mismatched =
            ext::install_from_source({ ext::SourceType::Archive, remote_url, "", "" }, store,
                                     ext::Origin::Developer, std::string(64, '0'));
    check(!mismatched.ok(), "a remote archive digest mismatch is refused");
    check(!mismatched.problems.empty()
                  && mismatched.problems.front().find("archive bytes") != std::string::npos,
          "the refusal names the archive bytes");

    // The same fixture over a real loopback HTTP connection, so the libcurl
    // download path (not just file://) is exercised end to end.
    LocalHttpServer server(read_file(remote_archive));
    check(server.ok(), "the loopback HTTP server starts");
    if (server.ok()) {
        const std::string http_url = server.url("/remote.tar");
        const ext::InstallResult http_plain = ext::install_from_source(
                { ext::SourceType::Archive, http_url, "", "" }, store, ext::Origin::Developer);
        check(http_plain.ok(),
              http_plain.problems.empty()
                      ? "an http archive installs"
                      : "an http archive installs: " + http_plain.problems.front());
        const ext::InstallResult http_verified =
                ext::install_from_source({ ext::SourceType::Archive, http_url, "", "" }, store,
                                         ext::Origin::Developer, remote_digest);
        check(http_verified.ok(),
              http_verified.problems.empty()
                      ? "the http archive digest verifies"
                      : "the http archive digest verifies: " + http_verified.problems.front());
    }

    // A traversal path is refused before anything is written.
    const std::filesystem::path bad_archive = kBase / "bad.tar";
    check(write_tar(bad_archive, { { "../escape", "outside" } }),
          "the traversal archive fixture is written");
    const ext::InstallResult refused =
            ext::install_from_source({ ext::SourceType::Archive, bad_archive.string(), "", "" },
                                     store, ext::Origin::Developer);
    check(!refused.ok(), "a traversal archive is refused");
    check(!refused.problems.empty()
                  && refused.problems.front().find("unsafe path") != std::string::npos,
          "the refusal names the unsafe path");

    std::filesystem::remove_all(kBase, ec);
    return genesis::test::summary();
}
