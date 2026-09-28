// SPDX-License-Identifier: GPL-3.0-or-later
// SPDX-FileCopyrightText: 2026 Genesis-0 contributors

#pragma once

#include <filesystem>
#include <functional>
#include <string>
#include <vector>

namespace genesis::workspace::update {

// One apply job: replace the running install at `target` with the verified
// staged version at `staged`, then relaunch `binary`. `root` is the update
// store whose markers (`current`/`pending`/`confirmed`/`backup`) record the
// swap. All paths are absolute; `staged` and `target` are directories (a
// single-file AppImage swap is the same rename, but the app's install layout
// is a directory tree in v1).
struct ApplyPlan
{
    std::filesystem::path root;
    std::filesystem::path target;
    std::filesystem::path staged;
    // The executable to relaunch after the swap; empty means "do not relaunch".
    std::filesystem::path binary;
    // The staged version, recorded in the `current` marker (the staged
    // directory's name).
    std::string version;
};

// Why an apply refused. ok() when there is nothing to refuse.
struct ApplyResult
{
    std::vector<std::string> problems;
    bool ok() const { return problems.empty(); }
};

// Seam: how the detached helper is launched. Production binds
// spawn_detached_helper; tests inject a fake so nothing forks or relaunches.
using Spawner = std::function<bool(const ApplyPlan &)>;

// Seam: how the swapped-in binary is relaunched after a successful swap.
// Production binds relaunch_binary; tests inject a fake that records the call.
using Relauncher = std::function<bool(const std::filesystem::path &binary)>;

// The detached helper's outcome.
struct SwapOutcome
{
    bool swapped = false; // target now holds the staged version
    bool relaunched = false; // the relauncher reported success
    std::filesystem::path backup; // target.old-<ts>; empty when not swapped
};

// The helper's work, runnable synchronously (tests) or from the detached child
// (spawn_detached_helper). Backs up `target`, moves `staged` in, writes the
// `current` and `backup` markers, then relaunches. A rename failure restores
// the backup before returning, so a half-applied swap is never left behind.
SwapOutcome run_helper_swap(const ApplyPlan &plan, const Relauncher &relaunch);

// Orchestrates an apply: refuses when the build cannot apply (per `gate`) or
// there is no verified-staged version to swap in, writes the `pending` marker,
// and hands the plan to `spawner`. `gate` returns an empty string when an apply
// may proceed, otherwise the human-readable refusal; a null gate reads the real
// build gate (apply_gate_reason).
ApplyResult apply(const std::filesystem::path &root, const std::filesystem::path &target,
                  const std::filesystem::path &staged, const std::filesystem::path &binary,
                  const Spawner &spawner, const std::function<std::string()> &gate = nullptr);

// Restores the backup an apply left behind and clears the apply markers,
// repointing `current` at the confirmed baseline when one exists. Returns false
// when there is no backup to restore.
bool rollback(const std::filesystem::path &root, const std::filesystem::path &target);

// The real spawner: forks, detaches via setsid, and runs the helper once the
// parent has exited (a pipe whose write end only the parent holds, so the
// child's read returns EOF exactly when the parent is gone). Returns true when
// the child spawned.
bool spawn_detached_helper(const ApplyPlan &plan, const Relauncher &relaunch);

// The real relauncher: fork+execs `binary`, detached. Returns false when the
// fork fails (an exec failure in the child exits 127, reported by the caller).
bool relaunch_binary(const std::filesystem::path &binary);

// The real build gate: empty when the updater is compiled in and a release key
// is configured, otherwise the matching disabled reason (the same two distinct
// reasons the notify-only surface reports).
std::string apply_gate_reason();

} // namespace genesis::workspace::update
