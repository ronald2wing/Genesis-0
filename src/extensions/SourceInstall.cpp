// SPDX-License-Identifier: GPL-3.0-or-later
// SPDX-FileCopyrightText: 2026 Genesis-0 contributors

#include "extensions/SourceInstall.h"

#include <archive.h>
#include <archive_entry.h>

#include <algorithm>
#include <array>
#include <atomic>
#include <chrono>
#include <cstdint>
#include <fstream>
#include <span>
#include <string>
#include <string_view>
#include <system_error>
#include <utility>
#include <vector>

#include "ai/Sha256.h"
#include "core/Hex.h"
#include "core/UrlFetch.h"
#include "extensions/Subprocess.h"

namespace genesis::extensions {

namespace {

// A 64-digit lowercase hex string (the canonical digest form). A catalog entry
// may carry any string for `sha256`, but only this shape can ever match a
// computed tree digest, so a malformed one is refused before any source work.
bool is_lower_hex64(std::string_view text)
{
    if (text.size() != 64) {
        return false;
    }
    for (const char c : text) {
        const bool hex = (c >= '0' && c <= '9') || (c >= 'a' && c <= 'f');
        if (!hex) {
            return false;
        }
    }
    return true;
}

// The 32-byte digest as 64 lowercase hex digits.
// One component of a `subdir` is safe when it is neither "." nor ".." and not
// empty, so a subdir can name a directory inside the clone but can never escape
// it (an absolute path or a `..` component). Empty segments (from a leading or
// doubled `/`) are rejected rather than silently folded.
bool is_safe_subdir(std::string_view subdir)
{
    if (subdir.empty()) {
        return true; // the repository root
    }
    if (subdir.front() == '/') {
        return false; // an absolute path escapes the clone
    }
    std::size_t start = 0;
    while (start <= subdir.size()) {
        const std::size_t slash = subdir.find('/', start);
        const std::string_view component = subdir.substr(
                start, slash == std::string_view::npos ? std::string_view::npos : slash - start);
        if (component.empty() || component == "." || component == "..") {
            return false;
        }
        if (slash == std::string_view::npos) {
            break;
        }
        start = slash + 1;
    }
    return true;
}

// A fresh, unique extraction/clone destination under the system temp dir. The
// timestamp plus a process-wide counter make two concurrent installs distinct
// without consulting the filesystem first.
std::filesystem::path fresh_source_dir()
{
    static std::atomic<std::uint64_t> counter{ 0 };
    const std::uint64_t n = counter.fetch_add(1);
    const auto stamp = std::chrono::steady_clock::now().time_since_epoch().count();
    return std::filesystem::temp_directory_path()
            / (".genesis-source-" + std::to_string(stamp) + "-" + std::to_string(n));
}

// The most an archive may extract: a hostile file must not be able to fill the
// disk or exhaust memory before the manifest is even validated.
constexpr std::size_t kMaxArchiveBytes = 256 * 1024 * 1024;
constexpr std::size_t kMaxArchiveEntries = 100000;

// An archive source may name a local path or a URL. libcurl handles all three
// schemes; a local path stays a plain filesystem read and keeps its tree digest.
bool is_remote_archive(std::string_view url)
{
    return url.starts_with("http://") || url.starts_with("https://") || url.starts_with("file://");
}

// The SHA-256 of the archive file's bytes, for a remote source. The catalog's
// digest for a downloaded archive pins the fetched bytes rather than the
// extracted tree, so a publisher can sign before unpacking.
std::optional<std::string> file_sha256(const std::filesystem::path &path)
{
    std::ifstream in(path, std::ios::binary);
    if (!in) {
        return std::nullopt;
    }
    ai::Sha256 hash;
    std::array<char, 64 * 1024> buffer{ };
    while (in) {
        in.read(buffer.data(), static_cast<std::streamsize>(buffer.size()));
        const std::streamsize count = in.gcount();
        if (count > 0) {
            hash.update(std::span<const std::uint8_t>(
                    reinterpret_cast<const std::uint8_t *>(buffer.data()),
                    static_cast<std::size_t>(count)));
        }
    }
    if (!in.eof()) {
        return std::nullopt;
    }
    return core::hex_encode(hash.digest());
}

// A path inside an archive is safe when it is relative, uses '/' separators,
// and has no ".." component. A leading '/', a backslash or a Windows
// drive/root all escape the destination, so they are refused. A trailing '/'
// (a directory entry) and "." components (a "./file" entry) are harmless and
// accepted; an empty interior component ("a//b") is refused.
bool safe_archive_path(std::string_view path)
{
    if (path.ends_with('/')) {
        path.remove_suffix(1); // the directory marker, not a path component
    }
    if (path.empty() || path.front() == '/' || path.find('\\') != std::string_view::npos) {
        return false;
    }
    const std::filesystem::path relative(path);
    if (relative.is_absolute() || relative.has_root_name() || relative.has_root_directory()) {
        return false;
    }
    std::size_t start = 0;
    while (start <= path.size()) {
        const std::size_t slash = path.find('/', start);
        const std::string_view component = path.substr(
                start, slash == std::string_view::npos ? std::string_view::npos : slash - start);
        if (component == "..") {
            return false;
        }
        if (component.empty() && slash != std::string_view::npos) {
            return false; // an interior "//" is not a path this extractor writes
        }
        if (slash == std::string_view::npos) {
            break;
        }
        start = slash + 1;
    }
    return true;
}

// Extracts a tar/zip/7z archive into `dest` by reading each entry and writing
// it under the destination. Symlinks, hard links and device nodes are refused:
// an extension is regular files and directories, and a link is the classic way
// an archive escapes its root even when the pathname looks safe.
bool extract_archive(const std::filesystem::path &archive_path, const std::filesystem::path &dest,
                     std::string &error)
{
    archive *reader = archive_read_new();
    if (reader == nullptr) {
        error = "cannot allocate the archive reader";
        return false;
    }
    archive_read_support_filter_all(reader);
    archive_read_support_format_all(reader);

    const std::string archive_name = archive_path.string();
    if (archive_read_open_filename(reader, archive_name.c_str(), 64 * 1024) != ARCHIVE_OK) {
        error = archive_error_string(reader) != nullptr ? archive_error_string(reader)
                                                        : "cannot open the archive";
        archive_read_free(reader);
        return false;
    }
    if (archive_read_has_encrypted_entries(reader) > 0) {
        error = "encrypted archives are not supported";
        archive_read_free(reader);
        return false;
    }

    std::error_code ec;
    std::filesystem::create_directories(dest, ec);
    if (ec) {
        error = "cannot create the extraction directory: " + dest.string();
        archive_read_free(reader);
        return false;
    }

    std::size_t entries = 0;
    std::uint64_t total = 0;
    archive_entry *entry = nullptr;
    for (;;) {
        const int header = archive_read_next_header(reader, &entry);
        if (header == ARCHIVE_EOF) {
            break;
        }
        if (header != ARCHIVE_OK) {
            error = archive_error_string(reader) != nullptr ? archive_error_string(reader)
                                                            : "cannot read the archive header";
            archive_read_free(reader);
            return false;
        }
        if (++entries > kMaxArchiveEntries) {
            error = "the archive has more than 100,000 entries";
            archive_read_free(reader);
            return false;
        }

        const char *raw_path = archive_entry_pathname(entry);
        if (raw_path == nullptr) {
            error = "the archive contains an entry without a path";
            archive_read_free(reader);
            return false;
        }
        if (!safe_archive_path(raw_path)) {
            error = "the archive contains an unsafe path: " + std::string(raw_path);
            archive_read_free(reader);
            return false;
        }
        if (archive_entry_hardlink(entry) != nullptr) {
            error = "the archive contains a hard link: " + std::string(raw_path);
            archive_read_free(reader);
            return false;
        }

        const std::filesystem::path target = dest / std::filesystem::path(raw_path);
        const auto type = archive_entry_filetype(entry);
        if (type == AE_IFDIR) {
            std::filesystem::create_directories(target, ec);
            if (ec) {
                error = "cannot create " + target.string();
                archive_read_free(reader);
                return false;
            }
            continue;
        }
        if (type != AE_IFREG) {
            error = "the archive contains a non-regular entry: " + std::string(raw_path);
            archive_read_free(reader);
            return false;
        }

        std::filesystem::create_directories(target.parent_path(), ec);
        if (ec) {
            error = "cannot create " + target.parent_path().string();
            archive_read_free(reader);
            return false;
        }
        std::ofstream out(target, std::ios::binary | std::ios::trunc);
        if (!out) {
            error = "cannot write " + target.string();
            archive_read_free(reader);
            return false;
        }

        const void *buffer = nullptr;
        std::size_t size = 0;
        la_int64_t offset = 0;
        for (;;) {
            const int data = archive_read_data_block(reader, &buffer, &size, &offset);
            if (data == ARCHIVE_EOF) {
                break;
            }
            if (data != ARCHIVE_OK) {
                error = archive_error_string(reader) != nullptr ? archive_error_string(reader)
                                                                : "cannot read the archive data";
                archive_read_free(reader);
                return false;
            }
            if (offset < 0 || static_cast<std::uint64_t>(offset) > kMaxArchiveBytes
                || size > kMaxArchiveBytes - static_cast<std::uint64_t>(offset)
                || total > kMaxArchiveBytes - size) {
                error = "the archive expands beyond 256 MiB";
                archive_read_free(reader);
                return false;
            }
            out.seekp(static_cast<std::streamoff>(offset));
            out.write(static_cast<const char *>(buffer), static_cast<std::streamsize>(size));
            if (!out) {
                error = "cannot write " + target.string();
                archive_read_free(reader);
                return false;
            }
            total += size;
        }
        out.close();
        if (!out) {
            error = "cannot write " + target.string();
            archive_read_free(reader);
            return false;
        }

        // A permission failure is not fatal: the content is installed, and a
        // native extension is loaded by the host rather than executed by it.
        std::error_code perm_ec;
        std::filesystem::permissions(
                target, static_cast<std::filesystem::perms>(archive_entry_perm(entry) & 0777),
                std::filesystem::perm_options::replace, perm_ec);
    }

    archive_read_free(reader);
    return true;
}

// Installs the tree at `tree`, optionally below `source.subdir`, after applying
// the entry's content digest. Shared by the git and archive sources: both end
// with a directory tree on disk and the ordinary install path.
InstallResult install_from_tree(const std::filesystem::path &tree, const InstallSource &source,
                                const std::filesystem::path &store_root, Origin origin,
                                const std::optional<std::string> &expected_sha256,
                                std::string_view kind)
{
    if (!is_safe_subdir(source.subdir)) {
        InstallResult result;
        result.problems.push_back("the " + std::string(kind)
                                  + " subdir is not a safe path inside the tree");
        return result;
    }

    const std::filesystem::path manifest_dir = source.subdir.empty() ? tree : tree / source.subdir;
    std::error_code dir_ec;
    if (!std::filesystem::is_directory(manifest_dir, dir_ec) || dir_ec) {
        InstallResult result;
        result.problems.push_back("the " + std::string(kind)
                                  + " subdir does not exist: " + source.subdir);
        return result;
    }

    if (expected_sha256) {
        const std::optional<std::string> actual = tree_sha256(manifest_dir);
        if (!actual) {
            InstallResult result;
            result.problems.push_back("cannot compute the " + std::string(kind)
                                      + " tree's content digest");
            return result;
        }
        if (*actual != *expected_sha256) {
            InstallResult result;
            result.problems.push_back("the " + std::string(kind)
                                      + " tree does not match the entry's sha256 (expected "
                                      + *expected_sha256 + ", got " + *actual + ")");
            return result;
        }
    }

    return install_extension(manifest_dir, store_root, origin, source);
}

} // namespace

std::optional<std::string> tree_sha256(const std::filesystem::path &dir)
{
    std::error_code ec;
    if (!std::filesystem::is_directory(dir, ec) || ec) {
        return std::nullopt;
    }

    // Collect (relative path, absolute path) for every regular file, walking
    // symlinks only for the iterator itself (a symlinked directory is not
    // recursed into; a symlinked file is skipped as not a regular file).
    std::vector<std::pair<std::string, std::filesystem::path>> files;
    std::filesystem::recursive_directory_iterator it(dir, std::filesystem::directory_options::none,
                                                     ec);
    const std::filesystem::recursive_directory_iterator end;
    for (; !ec && it != end; it.increment(ec)) {
        const std::filesystem::file_status status = it->symlink_status(ec);
        if (ec) {
            return std::nullopt;
        }
        if (status.type() != std::filesystem::file_type::regular) {
            continue; // directories are walked; symlinks/others are not content
        }
        const std::filesystem::path relative = std::filesystem::relative(it->path(), dir, ec);
        if (ec) {
            return std::nullopt;
        }
        files.emplace_back(relative.generic_string(), it->path());
    }
    if (ec) {
        return std::nullopt;
    }

    std::sort(files.begin(), files.end(),
              [](const auto &left, const auto &right) { return left.first < right.first; });

    genesis::ai::Sha256 hasher;
    const std::uint8_t nul = 0;
    std::array<char, 8192> buffer{ };
    for (const auto &[relative, path] : files) {
        hasher.update(std::span<const std::uint8_t>(
                reinterpret_cast<const std::uint8_t *>(relative.data()), relative.size()));
        hasher.update(std::span<const std::uint8_t>(&nul, 1));
        std::ifstream in(path, std::ios::binary);
        if (!in) {
            return std::nullopt;
        }
        while (in) {
            in.read(buffer.data(), static_cast<std::streamsize>(buffer.size()));
            const std::streamsize read = in.gcount();
            if (read > 0) {
                hasher.update(std::span<const std::uint8_t>(
                        reinterpret_cast<const std::uint8_t *>(buffer.data()),
                        static_cast<std::size_t>(read)));
            }
        }
        if (in.bad()) {
            return std::nullopt;
        }
    }
    return core::hex_encode(hasher.digest());
}

InstallResult install_from_source(const InstallSource &source,
                                  const std::filesystem::path &store_root, Origin origin,
                                  const std::optional<std::string> &expected_sha256)
{
    // A declared digest that is not 64 lowercase hex can never match a computed
    // tree digest; refuse before touching any source, with the reason spelled
    // out rather than a later "does not match".
    if (expected_sha256 && !is_lower_hex64(*expected_sha256)) {
        InstallResult result;
        result.problems.push_back("the entry's sha256 is not 64 lowercase hex digits");
        return result;
    }

    switch (source.type) {
    case SourceType::Builtin: {
        InstallResult result;
        result.problems.push_back("a builtin source re-seeds on startup; there is nothing to "
                                  "install from a source");
        return result;
    }
    case SourceType::Dir: {
        // Absolutize the recorded path (a profile import may carry a
        // relative one), then install directly. The absolute path is what
        // install_extension re-records, so the export stays reproducible.
        std::error_code ec;
        const std::filesystem::path absolute = std::filesystem::absolute(source.url, ec);
        const std::filesystem::path resolved = ec ? std::filesystem::path(source.url) : absolute;
        std::error_code dir_ec;
        if (source.url.empty() || !std::filesystem::is_directory(resolved, dir_ec) || dir_ec) {
            InstallResult result;
            result.problems.push_back("install source path does not exist: " + source.url);
            return result;
        }
        if (expected_sha256) {
            const std::optional<std::string> actual = tree_sha256(resolved);
            if (!actual) {
                InstallResult result;
                result.problems.push_back("cannot compute the directory's content digest: "
                                          + resolved.string());
                return result;
            }
            if (*actual != *expected_sha256) {
                InstallResult result;
                result.problems.push_back("the directory's content does not match the entry's "
                                          "sha256 (expected "
                                          + *expected_sha256 + ", got " + *actual + ")");
                return result;
            }
        }
        return install_extension(
                resolved, store_root, origin,
                InstallSource{ .type = SourceType::Dir, .url = resolved.string() });
    }
    case SourceType::Archive: {
        const bool remote = is_remote_archive(source.url);
        std::filesystem::path archive_path;
        std::filesystem::path download_dir;
        if (remote) {
            download_dir = fresh_source_dir();
            std::error_code dir_ec;
            std::filesystem::create_directories(download_dir, dir_ec);
            if (dir_ec) {
                InstallResult result;
                result.problems.push_back("cannot create the download directory: "
                                          + download_dir.string());
                return result;
            }
            archive_path = download_dir / "archive";
            std::string error;
            if (!core::download_file(source.url, archive_path, kMaxArchiveBytes, error)) {
                InstallResult result;
                result.problems.push_back("cannot download the archive: " + error);
                std::error_code ignored;
                std::filesystem::remove_all(download_dir, ignored);
                return result;
            }
            if (expected_sha256) {
                const std::optional<std::string> actual = file_sha256(archive_path);
                if (!actual) {
                    InstallResult result;
                    result.problems.push_back("cannot compute the archive's content digest");
                    std::error_code ignored;
                    std::filesystem::remove_all(download_dir, ignored);
                    return result;
                }
                if (*actual != *expected_sha256) {
                    InstallResult result;
                    result.problems.push_back("the archive bytes do not match the entry's sha256 "
                                              "(expected "
                                              + *expected_sha256 + ", got " + *actual + ")");
                    std::error_code ignored;
                    std::filesystem::remove_all(download_dir, ignored);
                    return result;
                }
            }
        } else {
            std::error_code ec;
            const std::filesystem::path absolute = std::filesystem::absolute(source.url, ec);
            archive_path = ec ? std::filesystem::path(source.url) : absolute;
            std::error_code file_ec;
            if (source.url.empty() || !std::filesystem::is_regular_file(archive_path, file_ec)
                || file_ec) {
                InstallResult result;
                result.problems.push_back("install archive does not exist: " + source.url);
                return result;
            }
        }

        const std::filesystem::path extract_dir = fresh_source_dir();
        std::string error;
        if (!extract_archive(archive_path, extract_dir, error)) {
            InstallResult result;
            result.problems.push_back("cannot extract the archive: " + error);
            std::error_code ignored;
            std::filesystem::remove_all(extract_dir, ignored);
            if (!download_dir.empty()) {
                std::filesystem::remove_all(download_dir, ignored);
            }
            return result;
        }

        // A remote digest pins the fetched bytes, already checked above; a local
        // archive keeps the extracted-tree digest. The other value would be a
        // different digest, so only one is checked.
        const std::optional<std::string> tree_digest = remote ? std::nullopt : expected_sha256;
        const InstallResult installed =
                install_from_tree(extract_dir, source, store_root, origin, tree_digest, "archive");
        std::error_code ignored;
        std::filesystem::remove_all(extract_dir, ignored);
        if (!download_dir.empty()) {
            std::filesystem::remove_all(download_dir, ignored);
        }
        return installed;
    }
    case SourceType::Git:
        break;
    }

    if (!is_safe_subdir(source.subdir)) {
        InstallResult result;
        result.problems.push_back("the git subdir is not a safe path inside the repository");
        return result;
    }

    const std::filesystem::path clone_dir = fresh_source_dir();
    std::vector<std::string> argv = { "git", "clone", "--depth", "1" };
    if (!source.ref.empty()) {
        argv.push_back("--branch");
        argv.push_back(source.ref);
    }
    argv.push_back(source.url);
    argv.push_back(clone_dir.string());

    const SubprocessResult run = run_subprocess(argv);
    if (!run.started) {
        InstallResult result;
        result.problems.push_back("cannot start git");
        return result;
    }
    if (!run.exited) {
        InstallResult result;
        result.problems.push_back("git clone was killed");
        std::error_code ignored;
        std::filesystem::remove_all(clone_dir, ignored);
        return result;
    }
    if (run.exit_status == 127) {
        InstallResult result;
        result.problems.push_back("git is not available");
        return result;
    }
    if (run.exit_status != 0) {
        InstallResult result;
        result.problems.push_back("git clone failed (exit " + std::to_string(run.exit_status)
                                  + "): " + run.stderr_text);
        std::error_code ignored;
        std::filesystem::remove_all(clone_dir, ignored);
        return result;
    }

    // The clone succeeded; the shared tree path resolves the subdir, checks the
    // digest and records the original url/ref/subdir, never the temporary
    // clone, so a config export stays reproducible.
    const InstallResult installed =
            install_from_tree(clone_dir, source, store_root, origin, expected_sha256, "git");
    std::error_code ignored;
    std::filesystem::remove_all(clone_dir, ignored);
    return installed;
}

} // namespace genesis::extensions
