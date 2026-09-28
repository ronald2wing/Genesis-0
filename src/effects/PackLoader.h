// SPDX-License-Identifier: GPL-3.0-or-later
// SPDX-FileCopyrightText: 2026 Genesis-0 contributors

#pragma once

#include <toml++/toml.hpp>

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

// Reads an effect.toml into the host schema. No engine type, no shader source:
// the manifest is data, the body is the engine's business.
LoadResult load_pack(std::string_view toml_text);

// Convenience over a path; reports I/O failures the same way.
LoadResult load_pack_file(const std::filesystem::path &path);

// Reads one effect table (the body of a `[[effect]]` block in an
// extension.toml, or the `[effect]` table of a standalone effect.toml) plus
// its scoped sub-tables into a Pack. `table` is the effect's own table;
// `path` is the diagnostic prefix (e.g. "effect[0]").
LoadResult load_effect_table(const toml::table &table, std::string_view path);

} // namespace genesis::effects
