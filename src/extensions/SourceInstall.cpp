// SPDX-License-Identifier: GPL-3.0-or-later
// SPDX-FileCopyrightText: 2026 Genesis-0 contributors

#include "extensions/SourceInstall.h"

#include <unistd.h>

#include <atomic>
#include <cstdint>
#include <string_view>
#include <system_error>

#include "extensions/Subprocess.h"

namespace genesis::extensions {

namespace {

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

InstallResult install_from_source(const InstallSource &source,
                                  const std::filesystem::path &store_root, Origin origin)
{
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

    const InstallResult installed = install_extension(
            manifest_dir, store_root, origin,
            InstallSource{ SourceType::Git, source.url, source.ref, source.subdir });
    std::error_code ignored;
    std::filesystem::remove_all(clone_dir, ignored);
    return installed;
}

} // namespace genesis::extensions
