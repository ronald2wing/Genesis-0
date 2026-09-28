// SPDX-License-Identifier: GPL-3.0-or-later
// SPDX-FileCopyrightText: 2026 Genesis-0 contributors

#pragma once

#include <filesystem>

#include "extensions/Install.h"

namespace genesis::extensions {

// Installs the extension named by `source`, whose type decides how the install
// directory is obtained before the ordinary install path runs:
//   - Dir: the `url` must name an existing directory; install runs directly.
//   - Git: clone `url` (optionally at `ref`) into a private temporary
//     directory, resolve `subdir` under it, and install from there.
//   - Builtin: refused (a builtin re-seeds on startup; nothing to reproduce).
// The trust gate and the source recording are install_extension's unchanged:
// a Dir install records the absolute path, a Git install records the original
// url/ref/subdir (never the temporary clone), so a config export stays
// reproducible. A Git source requires the `git` binary on PATH; a missing
// binary, a refused clone, or an unsafe/missing subdir is a refusal, reported
// in `problems`, never a process error.
InstallResult install_from_source(const InstallSource &source,
                                  const std::filesystem::path &store_root, Origin origin);

} // namespace genesis::extensions
