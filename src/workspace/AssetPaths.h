// SPDX-License-Identifier: GPL-3.0-or-later
// SPDX-FileCopyrightText: 2026 Genesis-0 contributors

#pragma once

#include <filesystem>

namespace genesis::workspace {

// The repository's `assets/` directory, resolved at runtime in this order:
//   1. the GENESIS_ASSET_DIR environment variable, when it names an existing
//      directory;
//   2. the compile-time GENESIS_ASSET_DIR macro (the source tree's assets/);
//   3. a walk up from the current working directory, then from the
//      executable's directory, looking for an `assets/` directory.
// Returns an empty path when none of the three resolve.
std::filesystem::path asset_root();

} // namespace genesis::workspace
