// SPDX-License-Identifier: GPL-3.0-or-later
// SPDX-FileCopyrightText: 2026 Genesis-0 contributors

#pragma once

#include <filesystem>

namespace genesis::workspace {

// The installed data root for a prefix layout: `<exe_dir>/../share/genesis-0`.
// This is the packaged counterpart to the beside-the-source-tree dev paths;
// asset_root() and the builtin-extension resolver share it so the packaged
// layout stays one contract. Returns the path whether or not it exists (the
// callers check existence). Empty when `exe_dir` is empty.
std::filesystem::path installed_share_root(const std::filesystem::path &exe_dir);

// The repository's `assets/` directory, resolved at runtime in this order:
//   1. the GENESIS_ASSET_DIR environment variable, when it names an existing
//      directory;
//   2. the compile-time GENESIS_ASSET_DIR macro (the source tree's assets/);
//   3. a walk up from the current working directory looking for an `assets/`
//      directory;
//   4. the packaged prefix layout, `<bindir>/../share/genesis-0/assets`;
//   5. a walk up from the executable's directory looking for an `assets/`
//      directory.
// Returns an empty path when none of the five resolve.
std::filesystem::path asset_root();

// The resolution core behind asset_root(), parameterized so tests drive every
// step with temp directories. `exe_dir` is the directory the running executable
// lives in (empty when unknown); `cwd` is where the walk-up starts;
// `env_override` is the GENESIS_ASSET_DIR value (nullptr reads nothing);
// `compile_time_dir` is the compile-time GENESIS_ASSET_DIR value (nullptr when
// this translation unit was built without it). Resolution order matches
// asset_root() above.
std::filesystem::path resolve_asset_root(const std::filesystem::path &exe_dir,
                                         const std::filesystem::path &cwd, const char *env_override,
                                         const char *compile_time_dir);

} // namespace genesis::workspace
