// SPDX-License-Identifier: GPL-3.0-or-later
// SPDX-FileCopyrightText: 2026 Genesis-0 contributors

#include "extensions/SourceInstall.h"

#include <unistd.h>

#include <algorithm>
#include <array>
#include <atomic>
#include <cstdint>
#include <fstream>
#include <span>
#include <string>
#include <string_view>
#include <system_error>
#include <utility>
#include <vector>

#include "ai/Sha256.h"
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
std::string hex_encode(std::span<const std::uint8_t> bytes)
{
    static constexpr char kDigits[] = "0123456789abcdef";
    std::string out(bytes.size() * 2, '0');
    for (std::size_t i = 0; i < bytes.size(); ++i) {
        out[2 * i] = kDigits[bytes[i] >> 4];
        out[2 * i + 1] = kDigits[bytes[i] & 0xf];
    }
    return out;
}

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

// A fresh, unique clone destination under the system temp dir. The pid plus a
// process-wide counter make two concurrent installs distinct without consulting
// the filesystem first.
std::filesystem::path fresh_clone_dir()
{
    static std::atomic<std::uint64_t> counter{ 0 };
    const std::uint64_t n = counter.fetch_add(1);
    return std::filesystem::temp_directory_path()
            / (".genesis-git-" + std::to_string(::getpid()) + "-" + std::to_string(n));
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
    return hex_encode(hasher.digest());
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
    case SourceType::Git:
        break;
    }

    if (!is_safe_subdir(source.subdir)) {
        InstallResult result;
        result.problems.push_back("the git subdir is not a safe path inside the repository");
        return result;
    }

    const std::filesystem::path clone_dir = fresh_clone_dir();
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

    // The clone succeeded; resolve the subdir (the repository root when empty)
    // and install from it. The recorded source is the original url/ref/subdir,
    // never the temporary clone, so a config export stays reproducible.
    const std::filesystem::path manifest_dir =
            source.subdir.empty() ? clone_dir : clone_dir / source.subdir;
    std::error_code dir_ec;
    if (!std::filesystem::is_directory(manifest_dir, dir_ec) || dir_ec) {
        InstallResult result;
        result.problems.push_back("the git subdir does not exist in the repository: "
                                  + source.subdir);
        std::error_code ignored;
        std::filesystem::remove_all(clone_dir, ignored);
        return result;
    }

    if (expected_sha256) {
        const std::optional<std::string> actual = tree_sha256(manifest_dir);
        if (!actual) {
            InstallResult result;
            result.problems.push_back("cannot compute the checked-out tree's content digest");
            std::error_code ignored;
            std::filesystem::remove_all(clone_dir, ignored);
            return result;
        }
        if (*actual != *expected_sha256) {
            InstallResult result;
            result.problems.push_back("the checked-out tree does not match the entry's sha256 "
                                      "(expected "
                                      + *expected_sha256 + ", got " + *actual + ")");
            std::error_code ignored;
            std::filesystem::remove_all(clone_dir, ignored);
            return result;
        }
    }

    const InstallResult installed = install_extension(
            manifest_dir, store_root, origin,
            InstallSource{ SourceType::Git, source.url, source.ref, source.subdir });
    std::error_code ignored;
    std::filesystem::remove_all(clone_dir, ignored);
    return installed;
}

} // namespace genesis::extensions
