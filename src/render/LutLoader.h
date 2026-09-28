// SPDX-License-Identifier: GPL-3.0-or-later
// SPDX-FileCopyrightText: 2026 Genesis-0 contributors

#pragma once

#include <filesystem>
#include <optional>
#include <string>
#include <vector>

#include "core/ShaderPass.h"

// Reading an Adobe .cube 3D LUT off disk into a `core::Lut`, the resource a
// pass binds as a 3D texture and the shader's `lut()` samples.
//
// The file format is the standard .cube 3D LUT layout: a few
// keyword lines - TITLE, LUT_3D_SIZE, DOMAIN_MIN, DOMAIN_MAX - then the table
// itself, one red/green/blue triple per row, red changing fastest, then
// green, then blue. That is exactly the order `Lut::from_rgb` takes its
// triples in, so a parsed file feeds the core type without any reordering.
//
// This module is only the reader: it turns text into a table. It does not
// decide which table a pack uses nor bind it - the resolver and the engine
// adapter own those steps.

namespace genesis::render {

// The outcome of loading one .cube file: the table, or a reason there is
// none. `problem` is empty exactly when `lut` holds a table; on any malformed
// input it names the first thing that was wrong, never a partial table.
struct LutResult
{
    std::optional<genesis::core::Lut> lut;
    std::string problem; // non-empty on failure
};

// Parses `path` into a `core::Lut`. A file that is missing, unreadable, or
// not a well-formed 3D LUT yields nullopt with a legible `problem` - never a
// crash. See the .cpp for the exact rules the reader enforces.
LutResult load_lut(const std::filesystem::path &path);

// Every `.cube` file under `root`, recursively, sorted by path: the set a
// grade picker would list. A root that is not a directory yields an empty
// list.
std::vector<std::filesystem::path> available_luts(const std::filesystem::path &root);

} // namespace genesis::render
