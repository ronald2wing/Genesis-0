// SPDX-License-Identifier: GPL-3.0-or-later
// SPDX-FileCopyrightText: 2026 Genesis-0 contributors

#pragma once

#include <filesystem>
#include <optional>
#include <string>
#include <string_view>

#include "core/ShaderPass.h"
#include "effects/Pack.h"
#include "project/model/Effects.h"

// The on-disk contract between a Pack's manifest and the shader body the
// engine runs.
//
// A picture Pack lives in a directory holding `effect.toml` (the manifest, read
// by PackLoader) and `effect.glsl` (the fragment BODY this module reads). The body is the tail `ShaderChain::fragment_source_for`
// appends after its prelude and the pass's parameter uniform declarations,
// so it must not declare a sampler2D (the prelude owns the one texture
// glshader binds) nor redeclare a parameter uniform.
//
// An `effect.wgsl` beside a manifest is IGNORED on this execution
// path: the engine runs GLSL, and a WGSL body handed to a glshader would
// fail to compile. `[wgsl].entry` is likewise not consulted - the body file
// is always `effect.glsl`. A pack that ships only `effect.wgsl` has no GLSL
// body and is reported rather than mis-run.

namespace genesis::effects {

// The body file's name, fixed beside `effect.toml`.
inline constexpr std::string_view GLSL_BODY = "effect.glsl";

// Reading a pack's body: the text, or a problem when `effect.glsl` is not
// there (which says so even when `effect.wgsl` is).
struct BodyResult
{
    std::optional<std::string> body;
    std::string problem; // non-empty on failure
};

// Reads the pack's `effect.glsl` body from `pack_dir`. `problem` is empty on
// success.
BodyResult read_body(const std::filesystem::path &pack_dir);

// A Pack resolved whole: the manifest loaded, the instance resolved to a
// ShaderPass, and its `source` set to the body. On failure the pass is
// nullopt and `problem` says why.
struct SourceResult
{
    std::optional<genesis::core::ShaderPass> pass;
    std::string problem; // non-empty on failure
};

SourceResult resolve_source(const std::filesystem::path &pack_dir,
                            const genesis::project::AppliedFilter &instance, double at);

// Resolves a Pack from an already-loaded Pack object plus its body path (not
// a directory, but the effect.glsl file). The Pack's manifest fields are
// already parsed; only the body is read from disk.
SourceResult resolve_body(const std::filesystem::path &body_path, const Pack &pack,
                          const genesis::project::AppliedFilter &instance, double at);

} // namespace genesis::effects
