// SPDX-License-Identifier: GPL-3.0-or-later
// SPDX-FileCopyrightText: 2026 Genesis-0 contributors

#include "workspace/update/Apply.h"

#include <cerrno>
#include <cstdint>
#include <ctime>
#include <string>
#include <system_error>

#include <fcntl.h>
#include <sys/types.h>
#include <unistd.h>

#include "workspace/update/Markers.h"
#include "workspace/update/Signature.h"
#include "workspace/update/Updater.h"

namespace genesis::workspace::update {

std::string apply_gate_reason()
{
    if (!updater_enabled()) {
        return "disabled: crypto not compiled in (rebuild with GENESIS_UPDATER=ON)";
    }
    if (!release_key_configured()) {
        return "disabled: release key not configured";
    }
    return { };
}

namespace {

// A sibling `target.old-<epoch seconds>` that does not already exist, so two
// applies within the same second never collide.
std::filesystem::path backup_path(const std::filesystem::path &target)
{
    std::error_code ec;
    for (std::uint64_t tick = static_cast<std::uint64_t>(std::time(nullptr));; ++tick) {
        const std::filesystem::path candidate =
                std::filesystem::path(target.string() + ".old-" + std::to_string(tick));
        if (!std::filesystem::exists(candidate, ec)) {
            return candidate;
        }
    }
}

// Points the child's stdio at /dev/null and detaches it into its own session,
// so the helper (or relaunched binary) neither inherits the app's terminal nor
// ties its lifetime to it.
void detach()
{
    ::setsid();
    const int devnull = ::open("/dev/null", O_RDWR);
    if (devnull < 0) {
        return;
    }
    ::dup2(devnull, STDIN_FILENO);
    ::dup2(devnull, STDOUT_FILENO);
    ::dup2(devnull, STDERR_FILENO);
    if (devnull > STDERR_FILENO) {
        ::close(devnull);
    }
}

} // namespace

SwapOutcome run_helper_swap(const ApplyPlan &plan, const Relauncher &relaunch)
{
    SwapOutcome outcome;

    // Back up the current install. A missing target (never installed, or
    // already applied) fails with nothing to restore.
    outcome.backup = backup_path(plan.target);
    std::error_code ec;
    std::filesystem::rename(plan.target, outcome.backup, ec);
    if (ec) {
        outcome.backup.clear();
        return outcome;
    }

    // Swap the staged version in; on failure restore the backup, so the install
    // is never left as a dangling backup with no target.
    std::filesystem::rename(plan.staged, plan.target, ec);
    if (ec) {
        std::error_code restore_ec;
        std::filesystem::rename(outcome.backup, plan.target, restore_ec);
        outcome.backup.clear();
        return outcome;
    }

    // Record the swap: `current` names the new version and `backup` records
    // where the old install went, so rollback() can restore it without
    // globbing. A marker write failure does not undo the swap - the swap is
    // authoritative, the markers are recovery aids.
    write_marker(plan.root / kCurrent, plan.version);
    write_marker(plan.root / kBackup, outcome.backup.string());

    outcome.swapped = true;
    if (!plan.binary.empty() && relaunch) {
        outcome.relaunched = relaunch(plan.binary);
    }
    return outcome;
}

ApplyResult apply(const std::filesystem::path &root, const std::filesystem::path &target,
                  const std::filesystem::path &staged, const std::filesystem::path &binary,
                  const Spawner &spawner, const std::function<std::string()> &gate)
{
    ApplyResult result;

    const std::string reason = gate ? gate() : apply_gate_reason();
    if (!reason.empty()) {
        result.problems.push_back(reason);
        return result;
    }

    if (staged.empty() || !std::filesystem::exists(staged)) {
        result.problems.push_back("no verified-staged version to apply");
        return result;
    }
    if (target.empty()) {
        result.problems.push_back("no install directory to update");
        return result;
    }

    const std::string version = staged.filename().string();
    // The apply gate respects the same anti-downgrade/replay check the install
    // path runs: a staged version at or below the last-seen record is refused
    // before the pending marker is written or the helper is spawned.
    if (const std::string refusal = monotonic_refusal(root, version); !refusal.empty()) {
        result.problems.push_back(refusal);
        return result;
    }
    if (!write_marker(root / kPending, version)) {
        result.problems.push_back("cannot write the pending marker");
        return result;
    }

    ApplyPlan plan{ root, target, staged, binary, version };
    if (!spawner || !spawner(plan)) {
        result.problems.push_back("could not start the update helper");
        return result;
    }
    return result;
}

bool rollback(const std::filesystem::path &root, const std::filesystem::path &target)
{
    const std::optional<std::string> backup = read_marker(root / kBackup);
    if (!backup || backup->empty()) {
        return false;
    }

    std::error_code ec;
    const std::filesystem::path backup_path(*backup);
    if (!std::filesystem::exists(backup_path, ec)) {
        remove_marker(root / kBackup);
        return false;
    }

    // Move the (possibly broken) new install aside, then restore the backup.
    // Best effort: if the move-aside fails, the restore rename over a non-empty
    // directory would also fail, so report it rather than half-restore.
    const std::filesystem::path failed = std::filesystem::path(
            target.string() + ".failed-" + std::to_string(std::time(nullptr)));
    if (std::filesystem::exists(target, ec)) {
        std::filesystem::rename(target, failed, ec);
        ec.clear();
    }
    std::filesystem::rename(backup_path, target, ec);
    if (ec) {
        // Try to put the failed install back rather than leave the dir missing.
        std::error_code putback_ec;
        if (std::filesystem::exists(failed, putback_ec)
            && !std::filesystem::exists(target, putback_ec)) {
            std::filesystem::rename(failed, target, putback_ec);
        }
        return false;
    }

    // Clear the apply markers and repoint `current` at the confirmed baseline
    // when one exists (a fresh install has none; `current` then keeps the
    // restored version's name).
    remove_marker(root / kBackup);
    remove_marker(root / kPending);
    if (const std::optional<std::string> confirmed = confirmed_version(root)) {
        write_marker(root / kCurrent, *confirmed);
    }
    return true;
}

bool spawn_detached_helper(const ApplyPlan &plan, const Relauncher &relaunch)
{
    int pipefd[2];
    if (::pipe(pipefd) != 0) {
        return false;
    }

    const pid_t pid = ::fork();
    if (pid < 0) {
        ::close(pipefd[0]);
        ::close(pipefd[1]);
        return false;
    }

    if (pid == 0) {
        // Child: detach, then block until the parent's write end closes (which
        // happens only when the parent exits), then perform the swap.
        ::close(pipefd[1]);
        detach();
        char byte;
        while (::read(pipefd[0], &byte, 1) < 0 && errno == EINTR) { }
        ::close(pipefd[0]);
        const SwapOutcome outcome = run_helper_swap(plan, relaunch);
        ::_exit(outcome.swapped ? 0 : 1);
    }

    // Parent: drop the read end and keep the write end open until this process
    // exits - that EOF is the helper's signal to proceed.
    ::close(pipefd[0]);
    return true;
}

bool relaunch_binary(const std::filesystem::path &binary)
{
    const pid_t pid = ::fork();
    if (pid < 0) {
        return false;
    }
    if (pid == 0) {
        detach();
        ::execl(binary.c_str(), binary.c_str(), static_cast<char *>(nullptr));
        ::_exit(127);
    }
    return true;
}

} // namespace genesis::workspace::update
