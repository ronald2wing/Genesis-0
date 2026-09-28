// SPDX-License-Identifier: GPL-3.0-or-later
// SPDX-FileCopyrightText: 2026 Genesis-0 contributors

// The self-update apply path's hermetic suite: the helper swap ordering, the
// backup-restore on failure, the refusal paths (disabled / nothing staged /
// no target / unwritable), the marker lifecycle, and rollback. Everything runs
// against temp directories with fake executables; the spawner and relauncher
// seams are fakes, so nothing forks or relaunches for real.

#include <cstdint>
#include <cstdio>
#include <filesystem>
#include <fstream>
#include <functional>
#include <optional>
#include <string>
#include <string_view>
#include <vector>

#include "workspace/update/Apply.h"
#include "workspace/update/Markers.h"
#include "workspace/update/Signature.h"
#include "workspace/update/Updater.h"
#include "../support/Checks.h"

namespace {

using genesis::test::check;

namespace upd = genesis::workspace::update;

std::filesystem::path scratch(std::string_view name)
{
    const std::filesystem::path dir =
            std::filesystem::temp_directory_path() / ("genesis-apply-" + std::string(name));
    std::error_code ec;
    std::filesystem::remove_all(dir, ec);
    std::filesystem::create_directories(dir, ec);
    return dir;
}

void write_file(const std::filesystem::path &path, std::string_view body)
{
    std::filesystem::create_directories(path.parent_path());
    std::ofstream out(path, std::ios::trunc);
    out << body;
}

std::string read_file(const std::filesystem::path &path)
{
    std::ifstream in(path);
    return std::string{ std::istreambuf_iterator<char>(in), std::istreambuf_iterator<char>() };
}

// The gate tests inject so the crypto-free pipeline is reached in a build
// without the updater runtime.
const std::function<std::string()> kEnabled = [] { return std::string(); };

// A relauncher fake: records the path it was asked to launch and reports the
// `result` the test wants.
struct FakeRelaunch
{
    bool result = true;
    std::optional<std::filesystem::path> launched;
};

upd::Relauncher fake_relaunch(FakeRelaunch &fake)
{
    return [&fake](const std::filesystem::path &binary) {
        fake.launched = binary;
        return fake.result;
    };
}

// A spawner fake: records the plan and reports `result`.
struct FakeSpawn
{
    bool result = true;
    std::optional<upd::ApplyPlan> plan;
};

upd::Spawner fake_spawn(FakeSpawn &fake)
{
    return [&fake](const upd::ApplyPlan &plan) {
        fake.plan = plan;
        return fake.result;
    };
}

// -- refusal paths ------------------------------------------------------------

void test_apply_refuses_when_disabled()
{
    const std::filesystem::path root = scratch("disabled");
    const std::filesystem::path target = root / "target";
    const std::filesystem::path staged = root / "staged";
    std::filesystem::create_directories(target);
    std::filesystem::create_directories(staged);

    FakeSpawn spawn;
    // The real build gate (null gate) refuses: either the crypto is not
    // compiled in (default build) or the release key is still all zeros
    // (enabled build). Both read "disabled".
    const upd::ApplyResult result = upd::apply(root, target, staged, { }, fake_spawn(spawn));
    check(!result.ok(), "apply refuses when the updater is disabled");
    check(!result.problems.empty() && result.problems.front().find("disabled") != std::string::npos,
          "the refusal names a disabled reason");
    check(!spawn.plan.has_value(), "no helper is spawned when disabled");
    check(!upd::pending_version(root).has_value(), "disabled apply writes no pending marker");
}

void test_apply_refuses_without_a_staged_version()
{
    const std::filesystem::path root = scratch("no-staged");
    const std::filesystem::path target = root / "target";
    std::filesystem::create_directories(target);

    FakeSpawn spawn;
    const upd::ApplyResult result =
            upd::apply(root, target, root / "missing", { }, fake_spawn(spawn), kEnabled);
    check(!result.ok(), "apply refuses without a staged version");
    check(!result.problems.empty() && result.problems.front().find("staged") != std::string::npos,
          "the refusal names the missing staged version");
    check(!spawn.plan.has_value(), "no helper is spawned without a staged version");
}

void test_apply_refuses_without_a_target()
{
    const std::filesystem::path root = scratch("no-target");
    const std::filesystem::path staged = root / "staged";
    std::filesystem::create_directories(staged);

    FakeSpawn spawn;
    const upd::ApplyResult result = upd::apply(root, { }, staged, { }, fake_spawn(spawn), kEnabled);
    check(!result.ok(), "apply refuses without a target install dir");
    check(!result.problems.empty() && result.problems.front().find("install") != std::string::npos,
          "the refusal names the missing install directory");
}

void test_apply_refuses_when_the_spawner_fails()
{
    const std::filesystem::path root = scratch("spawn-fail");
    const std::filesystem::path target = root / "target";
    const std::filesystem::path staged = root / "2.0.0";
    std::filesystem::create_directories(target);
    std::filesystem::create_directories(staged);

    FakeSpawn spawn;
    spawn.result = false;
    const upd::ApplyResult result =
            upd::apply(root, target, staged, { }, fake_spawn(spawn), kEnabled);
    check(!result.ok(), "apply refuses when the helper cannot be spawned");
    check(!result.problems.empty() && result.problems.front().find("helper") != std::string::npos,
          "the refusal names the spawn failure");
}

void test_apply_respects_the_monotonic_check()
{
    const std::filesystem::path root = scratch("apply-monotonic");
    const std::filesystem::path target = root / "target";
    std::filesystem::create_directories(target);

    // Seed the last-seen record at 2.0.0 (what has already run).
    upd::write_marker(root / upd::kLastSeen, "2.0.0");

    const std::filesystem::path old = root / "1.0.0";
    std::filesystem::create_directories(old);
    FakeSpawn spawn_old;
    const upd::ApplyResult downgrade =
            upd::apply(root, target, old, { }, fake_spawn(spawn_old), kEnabled);
    check(!downgrade.ok(), "apply refuses an older staged version");
    check(!downgrade.problems.empty()
                  && downgrade.problems.front().find("downgrade") != std::string::npos,
          "the refusal names a downgrade");
    check(!spawn_old.plan.has_value(), "no helper is spawned for a downgrade");
    check(!upd::pending_version(root).has_value(), "no pending marker is written for a downgrade");

    const std::filesystem::path same = root / "2.0.0";
    std::filesystem::create_directories(same);
    FakeSpawn spawn_same;
    const upd::ApplyResult replay =
            upd::apply(root, target, same, { }, fake_spawn(spawn_same), kEnabled);
    check(!replay.ok(), "apply refuses an equal staged version");
    check(!replay.problems.empty() && replay.problems.front().find("replay") != std::string::npos,
          "the refusal names a replay");
    check(!spawn_same.plan.has_value(), "no helper is spawned for a replay");

    const std::filesystem::path newer = root / "3.0.0";
    std::filesystem::create_directories(newer);
    FakeSpawn spawn_newer;
    const upd::ApplyResult accepted =
            upd::apply(root, target, newer, { }, fake_spawn(spawn_newer), kEnabled);
    check(accepted.ok(), "apply accepts a newer staged version");
    check(spawn_newer.plan.has_value(), "the helper is spawned for a newer version");
    check(upd::pending_version(root) == "3.0.0", "the newer version is pending");
}

// -- orchestration ------------------------------------------------------------

void test_apply_writes_pending_and_hands_off_the_plan()
{
    const std::filesystem::path root = scratch("orchestrate");
    const std::filesystem::path target = root / "target";
    const std::filesystem::path staged = root / "2.0.0";
    const std::filesystem::path binary = target / "bin" / "genesis-app";
    std::filesystem::create_directories(target);
    std::filesystem::create_directories(staged);
    write_file(staged / "payload", "the new version");

    FakeSpawn spawn;
    const upd::ApplyResult result =
            upd::apply(root, target, staged, binary, fake_spawn(spawn), kEnabled);
    check(result.ok(), "apply succeeds with a staged version and a working spawner");
    check(upd::pending_version(root) == "2.0.0", "apply writes the pending marker");

    check(spawn.plan.has_value(), "the spawner received a plan");
    if (spawn.plan) {
        check(spawn.plan->root == root, "the plan carries the marker root");
        check(spawn.plan->target == target, "the plan carries the install dir");
        check(spawn.plan->staged == staged, "the plan carries the staged version");
        check(spawn.plan->binary == binary, "the plan carries the binary to relaunch");
        check(spawn.plan->version == "2.0.0", "the plan carries the staged version string");
    }
}

// -- helper swap --------------------------------------------------------------

void test_helper_swaps_and_relaunches()
{
    const std::filesystem::path root = scratch("swap");
    const std::filesystem::path target = root / "target";
    const std::filesystem::path staged = root / "2.0.0";
    const std::filesystem::path binary = target / "genesis-app";
    std::filesystem::create_directories(target);
    std::filesystem::create_directories(staged);
    write_file(target / "payload", "the old version");
    write_file(staged / "payload", "the new version");

    FakeRelaunch relaunch;
    const upd::ApplyPlan plan{ root, target, staged, binary, "2.0.0" };
    const upd::SwapOutcome outcome = upd::run_helper_swap(plan, fake_relaunch(relaunch));

    check(outcome.swapped, "the helper reports the swap succeeded");
    check(outcome.relaunched, "the helper reports the relaunch succeeded");
    check(read_file(target / "payload") == "the new version", "target now holds the staged bytes");
    check(!outcome.backup.empty() && std::filesystem::exists(outcome.backup),
          "the backup path exists");
    check(read_file(outcome.backup / "payload") == "the old version",
          "the backup holds the old bytes");
    check(upd::current_version(root) == "2.0.0", "current points at the new version");
    check(upd::read_marker(root / upd::kBackup) == outcome.backup.string(),
          "the backup marker records the backup path");
    check(relaunch.launched == binary, "the relauncher was asked to launch the binary");
}

void test_helper_restores_the_backup_on_failure()
{
    const std::filesystem::path root = scratch("restore");
    const std::filesystem::path target = root / "target";
    const std::filesystem::path staged = root / "missing"; // rename staged -> target fails
    std::filesystem::create_directories(target);
    write_file(target / "payload", "the old version");

    FakeRelaunch relaunch;
    const upd::ApplyPlan plan{ root, target, staged, { }, "2.0.0" };
    const upd::SwapOutcome outcome = upd::run_helper_swap(plan, fake_relaunch(relaunch));

    check(!outcome.swapped, "the helper reports the swap failed");
    check(outcome.backup.empty(), "no backup path survives a failed swap");
    check(std::filesystem::exists(target / "payload")
                  && read_file(target / "payload") == "the old version",
          "the target is restored on a failed swap");
    check(!relaunch.launched.has_value(), "no relaunch happens on a failed swap");
}

void test_helper_never_relaunches_without_a_binary()
{
    const std::filesystem::path root = scratch("no-binary");
    const std::filesystem::path target = root / "target";
    const std::filesystem::path staged = root / "2.0.0";
    std::filesystem::create_directories(target);
    std::filesystem::create_directories(staged);

    FakeRelaunch relaunch;
    const upd::ApplyPlan plan{ root, target, staged, { }, "2.0.0" };
    const upd::SwapOutcome outcome = upd::run_helper_swap(plan, fake_relaunch(relaunch));
    check(outcome.swapped, "a swap without a binary still swaps");
    check(!outcome.relaunched && !relaunch.launched.has_value(),
          "no relaunch is attempted without a binary");
}

// -- rollback -----------------------------------------------------------------

void test_rollback_restores_the_backup_and_clears_markers()
{
    const std::filesystem::path root = scratch("rollback");
    const std::filesystem::path target = root / "target";
    const std::filesystem::path staged = root / "2.0.0";
    std::filesystem::create_directories(target);
    std::filesystem::create_directories(staged);
    write_file(target / "payload", "the old version");
    write_file(staged / "payload", "the new version");

    // A prior successful apply: current=2.0.0, pending=2.0.0, confirmed=1.0.0.
    upd::write_marker(root / upd::kCurrent, "2.0.0");
    upd::write_marker(root / upd::kPending, "2.0.0");
    upd::write_marker(root / upd::kConfirmed, "1.0.0");

    FakeRelaunch relaunch;
    const upd::ApplyPlan plan{ root, target, staged, { }, "2.0.0" };
    check(upd::run_helper_swap(plan, fake_relaunch(relaunch)).swapped, "the swap runs first");
    check(read_file(target / "payload") == "the new version", "target is the new version");

    check(upd::rollback(root, target), "rollback succeeds");
    check(read_file(target / "payload") == "the old version", "rollback restores the old install");
    check(upd::current_version(root) == "1.0.0", "current repoints at the confirmed baseline");
    check(!upd::pending_version(root).has_value(), "rollback clears the pending marker");
    check(!upd::read_marker(root / upd::kBackup).has_value(), "rollback clears the backup marker");
}

void test_rollback_without_a_backup_fails()
{
    const std::filesystem::path root = scratch("no-backup");
    check(!upd::rollback(root, root / "target"), "rollback with no backup marker reports failure");
}

// -- marker lifecycle ---------------------------------------------------------

void test_apply_markers_follow_the_lifecycle()
{
    const std::filesystem::path root = scratch("markers");
    const std::filesystem::path target = root / "target";
    std::filesystem::create_directories(target);

    check(!upd::current_version(root).has_value(), "no current marker before any apply");
    check(!upd::pending_version(root).has_value(), "no pending marker before any apply");
    check(!upd::confirmed_version(root).has_value(), "no confirmed marker before any apply");

    const std::filesystem::path staged = root / "2.0.0";
    std::filesystem::create_directories(staged);
    FakeSpawn spawn;
    check(upd::apply(root, target, staged, { }, fake_spawn(spawn), kEnabled).ok(),
          "the first apply orchestrates");

    // Simulate the helper's swap and a subsequent confirm (the launcher's job
    // once the swapped-in version has run).
    FakeRelaunch relaunch;
    const upd::ApplyPlan plan{ root, target, staged, { }, "2.0.0" };
    check(upd::run_helper_swap(plan, fake_relaunch(relaunch)).swapped, "the helper swaps");
    upd::confirm(root);
    check(upd::confirmed_version(root) == "2.0.0", "confirm records the applied version");
    check(!upd::pending_version(root).has_value(), "confirm clears the pending marker");
}

} // namespace

int main()
{
    test_apply_refuses_when_disabled();
    test_apply_refuses_without_a_staged_version();
    test_apply_refuses_without_a_target();
    test_apply_refuses_when_the_spawner_fails();
    test_apply_respects_the_monotonic_check();
    test_apply_writes_pending_and_hands_off_the_plan();
    test_helper_swaps_and_relaunches();
    test_helper_restores_the_backup_on_failure();
    test_helper_never_relaunches_without_a_binary();
    test_rollback_restores_the_backup_and_clears_markers();
    test_rollback_without_a_backup_fails();
    test_apply_markers_follow_the_lifecycle();

    return genesis::test::summary();
}
