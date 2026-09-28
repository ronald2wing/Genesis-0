// SPDX-License-Identifier: GPL-3.0-or-later
// SPDX-FileCopyrightText: 2026 Genesis-0 contributors

#pragma once

#include <filesystem>
#include <optional>
#include <string>

namespace genesis::workspace {

// A project folder is claimed by the process that has it open, so two
// processes never edit the same folder's manifest at once. The claim is an
// advisory flock on `<root>/.genesis.lock`; the kernel drops the lock when the
// holding process dies, so a stale lock (a crash, a kill -9) reclaims itself on
// the next open - no PID/age scan, no cleanup pass. The lock file is created
// empty and is authoritative only by its existence and the flock: its contents
// are never written or read.

// Acquires the claim for a project folder `root`, replacing any previously
// held claim. The new claim is taken before the old one is dropped, so a failed
// open never loses the current project's claim. Reclaiming the same folder is
// a no-op. Returns nullopt on success, or why the claim was refused (another
// live process holds it).
std::optional<std::string> claim_project(const std::filesystem::path &root);

// Drops the held claim, if any. Releasing a folder the process does not hold
// is a no-op.
void release_project();

// Acquires the claim a project path implies: a directory claims itself; a file
// claims its parent when that parent is a project folder; anything else holds
// no claim (a standalone document has no folder to lock). When no folder is
// implied, any current claim is released. Returns nullopt on success or a
// no-claim path, or why the claim was refused.
std::optional<std::string> claim_project_path(const std::filesystem::path &path);

} // namespace genesis::workspace
