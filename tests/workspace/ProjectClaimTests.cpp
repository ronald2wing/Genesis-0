// SPDX-License-Identifier: GPL-3.0-or-later
// SPDX-FileCopyrightText: 2026 Genesis-0 contributors
//
// The project-claim suite: the advisory flock a process takes over a project
// folder so two processes never edit the same manifest at once. Acquire,
// in-process conflict (flock is per open-file-description, so a second open()
// is refused even here), release, replace, and stale reclaim - a holder that
// dies (fd closed) releases the lock with no scan or cleanup pass. Also the
// claim_project_path resolution (dir claims itself; a file claims its parent
// when that is a project; a standalone document claims nothing) and the
// failed-open-keeps-the-current-claim ordering. Hermetic: temp folders only.

#include <fcntl.h>
#include <sys/file.h>
#include <unistd.h>

#include <cstdio>
#include <filesystem>
#include <fstream>
#include <string>

#include "../support/Checks.h"

#include "project/model/VideoSettings.h"
#include "workspace/ProjectClaim.h"
#include "workspace/Projects.h"

namespace workspace = genesis::workspace;
using genesis::test::check;
using genesis::test::summary;

namespace {

struct Harness
{
    // A per-run unique root: one directory per process so concurrent test runs
    // never share (and never delete) each other's temp folders.
    std::filesystem::path root = std::filesystem::temp_directory_path()
            / ("genesis-project-claim-test-" + std::to_string(static_cast<long long>(::getpid())));

    Harness()
    {
        std::filesystem::remove_all(root);
        std::filesystem::create_directories(root);
        workspace::release_project();
    }

    ~Harness()
    {
        workspace::release_project();
        std::filesystem::remove_all(root);
    }

    // Creates a real project folder under the harness root and returns its path.
    std::filesystem::path make_project(const std::string &name)
    {
        const auto created =
                workspace::create_project(root, name, genesis::project::VideoSettings{ });
        check(created.has_value(), "create_project succeeds");
        return std::filesystem::path(created->path);
    }

    std::filesystem::path lock_file(const std::filesystem::path &project)
    {
        return project / ".genesis.lock";
    }
};

// Whether a fresh open file description can take the exclusive lock. Used to
// probe the lock from a second "process" (in-process, but flock is per
// open-file-description, so a second open() conflicts with the claim's own).
bool lock_is_taken(const std::filesystem::path &lock_file)
{
    const int fd = ::open(lock_file.c_str(), O_RDWR | O_CREAT, 0644);
    if (fd < 0) {
        return true; // cannot probe; treat as held
    }
    const bool taken = ::flock(fd, LOCK_EX | LOCK_NB) != 0;
    if (!taken) {
        ::flock(fd, LOCK_UN);
    }
    ::close(fd);
    return taken;
}

void test_acquire_and_hold()
{
    Harness harness;
    const std::filesystem::path project = harness.make_project("acquire");

    check(!workspace::claim_project(project).has_value(), "claim succeeds");
    check(std::filesystem::exists(harness.lock_file(project)), "the lock file exists");
    check(lock_is_taken(harness.lock_file(project)), "the lock is taken");
}

void test_conflict_is_refused()
{
    Harness harness;
    const std::filesystem::path project = harness.make_project("conflict");

    check(!workspace::claim_project(project).has_value(), "first claim succeeds");
    check(lock_is_taken(harness.lock_file(project)),
          "a second open() is refused while the claim is held");
}

void test_release()
{
    Harness harness;
    const std::filesystem::path project = harness.make_project("release");

    check(!workspace::claim_project(project).has_value(), "claim succeeds");
    workspace::release_project();
    check(!lock_is_taken(harness.lock_file(project)), "the lock is free after release");
}

void test_replace()
{
    Harness harness;
    const std::filesystem::path first = harness.make_project("first");
    const std::filesystem::path second = harness.make_project("second");

    check(!workspace::claim_project(first).has_value(), "first claim succeeds");
    check(!workspace::claim_project(second).has_value(), "the second claim replaces the first");
    check(lock_is_taken(harness.lock_file(second)), "the new claim is held");
    check(!lock_is_taken(harness.lock_file(first)), "the old folder's lock is free");
}

void test_stale_lock_is_reclaimed()
{
    Harness harness;
    const std::filesystem::path project = harness.make_project("stale");

    // A holder that dies: it takes the lock on its own file description, then
    // closes the fd without unlocking - exactly what the kernel does on process
    // death. There is no cleanup scan; the lock simply frees itself.
    const int dead = ::open(harness.lock_file(project).c_str(), O_RDWR | O_CREAT, 0644);
    check(dead >= 0, "the simulated holder opens the lock");
    check(::flock(dead, LOCK_EX | LOCK_NB) == 0, "the simulated holder takes the lock");
    ::close(dead);

    check(!workspace::claim_project(project).has_value(), "a stale lock is reclaimable");
    check(lock_is_taken(harness.lock_file(project)), "the reclaimed claim is held");
}

void test_reopen_is_a_noop()
{
    Harness harness;
    const std::filesystem::path project = harness.make_project("reopen");

    check(!workspace::claim_project(project).has_value(), "first claim succeeds");
    check(!workspace::claim_project(project).has_value(), "reclaiming the same folder is a no-op");
    check(lock_is_taken(harness.lock_file(project)), "the claim is still held");
}

void test_claim_project_path_resolution()
{
    Harness harness;
    const std::filesystem::path project = harness.make_project("resolution");
    const std::filesystem::path doc = project / "scene.json";
    {
        std::ofstream out(doc);
        out << "{}";
    }
    const std::filesystem::path standalone = harness.root / "standalone.json";
    {
        std::ofstream out(standalone);
        out << "{}";
    }

    check(!workspace::claim_project_path(project).has_value(), "a directory claims itself");
    check(lock_is_taken(harness.lock_file(project)), "the directory's claim is held");

    check(!workspace::claim_project_path(doc).has_value(), "a file claims its project parent");
    check(lock_is_taken(harness.lock_file(project)), "the parent's claim is held");

    // A standalone document (parent is not a project) holds no folder claim and
    // releases whatever was held.
    check(!workspace::claim_project_path(standalone).has_value(),
          "a standalone document claims nothing");
    check(!lock_is_taken(harness.lock_file(project)), "the previous claim is released");
}

void test_failed_open_keeps_the_current_claim()
{
    Harness harness;
    const std::filesystem::path open = harness.make_project("open");
    const std::filesystem::path foreign = harness.make_project("foreign");

    const auto first = workspace::open_project(open);
    check(first.has_value(), "opening the first project succeeds");
    check(lock_is_taken(harness.lock_file(open)), "the first project is claimed");

    // A second process already holds `foreign`.
    const int other = ::open(harness.lock_file(foreign).c_str(), O_RDWR | O_CREAT, 0644);
    check(other >= 0, "the other process opens the foreign lock");
    check(::flock(other, LOCK_EX | LOCK_NB) == 0, "the other process holds the foreign lock");

    const auto refused = workspace::open_project(foreign);
    check(!refused.has_value(), "opening a claimed project is refused");
    check(lock_is_taken(harness.lock_file(open)),
          "the failed open leaves the current claim intact");

    ::flock(other, LOCK_UN);
    ::close(other);
}

} // namespace

int main()
{
    test_acquire_and_hold();
    test_conflict_is_refused();
    test_release();
    test_replace();
    test_stale_lock_is_reclaimed();
    test_reopen_is_a_noop();
    test_claim_project_path_resolution();
    test_failed_open_keeps_the_current_claim();

    return summary();
}
