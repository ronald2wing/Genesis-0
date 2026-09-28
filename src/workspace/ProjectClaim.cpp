// SPDX-License-Identifier: GPL-3.0-or-later
// SPDX-FileCopyrightText: 2026 Genesis-0 contributors

#include "workspace/ProjectClaim.h"

#include <fcntl.h>
#include <sys/file.h>
#include <unistd.h>

#include <cerrno>
#include <cstring>
#include <mutex>
#include <string>

#include "workspace/Projects.h"

namespace genesis::workspace {

namespace {

// The one claim a process holds. The host is a single-Editor shell, so at most
// one project folder is open at a time; the state is process-global and guarded
// because the app and the CLI each call in from their own thread.
struct ClaimState
{
    std::mutex mutex;
    int fd = -1;
    std::string root; // the canonical string of the held folder
};

ClaimState &claim_state()
{
    static ClaimState state;
    return state;
}

// A canonical, comparison-stable spelling of a folder: symlinks resolved where
// they exist, lexically normalised where they do not. Two spellings of the same
// folder then compare equal, so re-opening a project never self-conflicts.
std::filesystem::path canonical_root(const std::filesystem::path &path)
{
    std::error_code error;
    const std::filesystem::path resolved = std::filesystem::weakly_canonical(path, error);
    if (!error) {
        return resolved;
    }
    return std::filesystem::absolute(path, error).lexically_normal();
}

// The lock file a project folder's claim lives on.
std::filesystem::path lock_path(const std::filesystem::path &root)
{
    return root / ".genesis.lock";
}

// The folder a path claims: a directory claims itself; a file claims its parent
// when that parent is a project folder; anything else claims nothing.
std::optional<std::filesystem::path> claim_root(const std::filesystem::path &path)
{
    if (std::filesystem::is_directory(path)) {
        return path;
    }
    if (std::filesystem::is_regular_file(path)) {
        const std::filesystem::path parent = path.parent_path();
        if (is_project(parent)) {
            return parent;
        }
    }
    return std::nullopt;
}

} // namespace

std::optional<std::string> claim_project(const std::filesystem::path &root)
{
    const std::filesystem::path target = canonical_root(root);
    const std::string target_string = target.string();

    ClaimState &state = claim_state();
    std::lock_guard<std::mutex> lock(state.mutex);

    // Already holding this folder: re-opening is a no-op, not a self-conflict
    // (flock is per open-file-description, so a second open() would otherwise
    // refuse its own process's lock).
    if (state.fd >= 0 && state.root == target_string) {
        return std::nullopt;
    }

    const int fd = ::open(lock_path(target).c_str(), O_RDWR | O_CREAT | O_CLOEXEC, 0644);
    if (fd < 0) {
        return "could not open the project lock (" + std::string(std::strerror(errno)) + ")";
    }
    if (::flock(fd, LOCK_EX | LOCK_NB) != 0) {
        ::close(fd);
        return "the project is already open in another process";
    }

    // The new claim is held; only now drop the old one so a failed open (the
    // caller's) never costs the current project its claim.
    if (state.fd >= 0) {
        ::close(state.fd);
    }
    state.fd = fd;
    state.root = target_string;
    return std::nullopt;
}

void release_project()
{
    ClaimState &state = claim_state();
    std::lock_guard<std::mutex> lock(state.mutex);
    if (state.fd >= 0) {
        ::close(state.fd);
        state.fd = -1;
        state.root.clear();
    }
}

std::optional<std::string> claim_project_path(const std::filesystem::path &path)
{
    const std::optional<std::filesystem::path> root = claim_root(path);
    if (!root) {
        // A standalone document (or a path that is not a project) holds no
        // folder claim; drop any previous one.
        release_project();
        return std::nullopt;
    }
    return claim_project(*root);
}

} // namespace genesis::workspace
