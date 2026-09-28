// SPDX-License-Identifier: GPL-3.0-or-later
// SPDX-FileCopyrightText: 2026 Genesis-0 contributors

#pragma once

#include <filesystem>
#include <string>
#include <vector>

namespace genesis::extensions {

// The builtin extension source directory, resolved at runtime in this order:
//   1. the GENESIS_BUILTIN_EXTENSIONS_DIR compile-time define (the source
//      tree's extensions/builtin/, so a developer build needs no install step);
//   2. a search beside the running executable (the dev build copies
//      extensions/builtin/ next to the binary);
//   3. the packaged prefix layout, `<bindir>/../share/genesis-0/extensions/builtin`.
// Returns an empty path when none of the three resolve.
std::filesystem::path builtin_root();

// The resolution core behind builtin_root(), parameterized so tests drive every
// step with temp directories. `exe_dir` is the directory the running executable
// lives in (empty when unknown); `compile_time_dir` is the compile-time
// GENESIS_BUILTIN_EXTENSIONS_DIR value (nullptr when this translation unit was
// built without it). Resolution order matches builtin_root() above.
std::filesystem::path resolve_builtin_root(const std::filesystem::path &exe_dir,
                                           const char *compile_time_dir);

// Seeds `store_root` with the manifest-validated builtins under `builtin_root`
// (one subdirectory `<id>` per builtin). For each builtin directory:
//   - a directory with no valid manifest is skipped and reported;
//   - an id on the store's removed list (removed.json) is skipped forever, so
//     a user's removal never silently reappears;
//   - an id already present in the store is left untouched (idempotent);
//   - otherwise the builtin installs under `<store>/<id>/<version>` and its
//     origin is recorded as Builtin.
// Best-effort: a failed install is reported, never fatal. Returns the problems
// found (empty when every builtin seeded or skipped).
std::vector<std::string> seed(const std::filesystem::path &store_root,
                              const std::filesystem::path &builtin_root);

} // namespace genesis::extensions
