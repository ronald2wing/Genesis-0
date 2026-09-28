// SPDX-License-Identifier: GPL-3.0-or-later
// SPDX-FileCopyrightText: 2026 Genesis-0 contributors

#pragma once

#include <functional>
#include <optional>
#include <string>
#include <vector>

#include "core/ShaderPass.h"
#include "effects/Pack.h"
#include "effects/Resolve.h"
#include "project/model/Clip.h"

// The R9 fallback: a clip's effect chain resolved to passes, with every link
// the resolver could not honour recorded rather than fatal. A missing or
// unsupported effect keeps the project intact and the rest of the chain runs
// (docs/architecture.md R9; docs/decisions/effect-execution.md §6).

namespace genesis::render {

// One effect a resolution had to leave out, and why. R9: a missing or
// unsupported effect is omitted, never fatal - the project is unchanged and
// the rest of the chain still runs.
struct EffectSkip
{
    std::string clip_id;
    std::string pack_id;
    std::string reason;
    bool operator==(const EffectSkip &) const = default;
};

// The passes a clip's effect chain resolves to, and every link that had to be
// skipped. `lookup` supplies a Pack by id (null when the catalogue does not
// know it) - injected rather than global so this is testable and the real
// store lands later without a rewrite.
struct ChainResolution
{
    std::vector<genesis::core::ShaderPass> passes;
    std::vector<EffectSkip> skips;
    // The CPU fallbacks the chain resolved, for the links whose Pack declares
    // one and whose element the caller's `available` probe found. Only filled
    // when `resolve_effect_chain` is handed a probe - the engine-free callers
    // leave it empty and keep the R9 skip.
    std::vector<genesis::effects::ResolvedCpuFallback> cpu;
};

// Resolves a clip's effect chain at time `at`. `lookup` maps a pack id to its
// Pack (null when the catalogue does not know it) - injected rather than
// global so this is testable and the real store lands later without a rewrite.
// A pointer, not a copy: the per-frame resolve dereferences the catalogue's
// own Pack rather than deep-copying it. `available` - when supplied - probes
// whether a named GStreamer element exists on this host: a Pack whose GPU path
// cannot express it but which declares a CPU fallback is then emitted as a
// `cpu` entry instead of an R9 skip, provided the element is present. An empty
// `available` preserves the engine-free behaviour: the fallback is never
// checked, and every not-expressible link is skipped as before.
ChainResolution resolve_effect_chain(
        const genesis::project::Clip &clip, double at,
        const std::function<const genesis::effects::Pack *(const std::string &)> &lookup,
        const std::function<bool(const std::string &)> &available = { });

} // namespace genesis::render
