// SPDX-License-Identifier: GPL-3.0-or-later
// SPDX-FileCopyrightText: 2026 Genesis-0 contributors
//
// The sidecar that records the key a cache file was written for: the data
// file's own path with ".key" appended. The writer (render::MediaCache) and the
// sweeper (workspace::CacheSweep) must agree on the spelling, so it lives here
// rather than as a copy in each.

#pragma once

#include <filesystem>
#include <string>

namespace genesis::core {

inline std::filesystem::path cache_sidecar_path(const std::filesystem::path &data_file)
{
    return std::filesystem::path(data_file.string() + ".key");
}

} // namespace genesis::core
