// SPDX-License-Identifier: GPL-3.0-or-later
// SPDX-FileCopyrightText: 2026 Genesis-0 contributors
//
// Derived from Concat's concat-effects/src/manifest.rs,
// SPDX-License-Identifier: GPL-3.0-or-later,
// SPDX-FileCopyrightText: 2026 Jareer and Concat contributors.

#pragma once

#include <filesystem>
#include <optional>
#include <string>
#include <string_view>
#include <vector>

#include "effects/Pack.h"

namespace genesis::effects {

// What went wrong reading a manifest. A malformed Pack is reported, never
// guessed at: a manifest that does not validate is not loadable.
struct LoadError
{
    std::string where; // the offending key/table, for a human
    std::string what; // what was wrong
};

// The pack and the problems found. A non-empty `problems` means the pack is
// unusable; an empty one means it validated.
struct LoadResult
{
    std::optional<Pack> pack;
    std::vector<LoadError> problems;
};

// Reads an effect.toml (format 2) into the host schema. No engine type, no
// shader source: the manifest is data, the body is the engine's business.
LoadResult load_pack(std::string_view toml_text);

// Convenience over a path; reports I/O failures the same way.
LoadResult load_pack_file(const std::filesystem::path &path);

} // namespace genesis::effects
