// SPDX-License-Identifier: GPL-3.0-or-later
// SPDX-FileCopyrightText: 2026 Genesis-0 contributors
//
// Derived from Concat's concat-effects/src/catalogue.rs and shader.rs,
// SPDX-License-Identifier: GPL-3.0-or-later,
// SPDX-FileCopyrightText: 2026 Jareer and Concat contributors.

#pragma once

#include <optional>
#include <string>
#include <vector>

#include "core/ShaderPass.h"
#include "effects/Pack.h"
#include "project/model/Effects.h"

// The resolver: a loaded Pack plus one clip's instance of it, turned into the
// host's executable pass data.
//
// The schema (`Pack`) declares what an effect *is*; the resolved payload
// (`core::ShaderPass`) is what the engine adapter runs. This module is the
// seam between them (docs/decisions/effects-execution.md §7): it lays the
// instance's parameter values into the uniform buffer at the schema's offsets,
// forms the pipeline cache key, and refuses a Pack the chosen execution path
// cannot express. No engine type and no shader source appear here - the GLSL
// body is composed by the adapter, not resolved from the schema.

namespace genesis::effects {

// Whether a loaded Pack can run on the chosen execution path. The multi-pass
// prototype settled that a pass reading a named intermediate is NOT
// expressible with the stock GL elements, so such a Pack is not runnable as
// written - the caller takes the R9 fallback (keep the project intact, the
// effect simply does not apply) rather than failing the edit.
bool is_expressible(const Pack &pack);

// Resolves one instance of `pack` (its parameter values, resolved at `at`
// where the instance animates them) into the host's executable pass data.
// Returns nullopt when the pack is not expressible on this path, when it is a
// transition (whose combine is `resolve_transition`'s business), or when the
// instance names parameters the pack does not declare.
//
// A non-null `reveal_map` is attached to the pass as its bound reveal texture:
// the app supplies it for a title clip's reveal effect, and every other caller
// passes nothing so the pass carries no map (RevealMap::identity is bound).
// Attaching here - the single place a ShaderPass is born from a Pack - means a
// map, once supplied, rides every pass uniformly rather than being keyed by a
// clip id this function never sees.
//
// A colour-look pack declares an enum parameter named `lut`, whose value is an
// index into the sorted `.cube` tables under asset_root()/`luts`; the resolver
// loads the table at that index into `pass.lut`, and leaves it unset on any
// failure so the pack's GLSL approximation stays the in-engine fallback.
std::optional<genesis::core::ShaderPass>
resolve_pass(const Pack &pack, const genesis::project::AppliedFilter &instance, double at,
             std::optional<genesis::core::RevealMap> reveal_map = std::nullopt);

// The same for a transition Pack: the pass the compositor runs with `progress`
// through the cut. Returns nullopt when the pack has no transition form, or
// when the instance names parameters the pack does not declare.
std::optional<genesis::core::TransitionPass>
resolve_transition(const Pack &pack, const genesis::project::AppliedFilter &instance,
                   double progress);

// One element property of a resolved CPU fallback, at its evaluated value.
struct CpuFallbackValue
{
    // The element property name, e.g. `brightness` on `frei0r-filter-brightness`.
    std::string key;
    // The value to set it to, evaluated from the instance's parameters.
    double value;
};

// A CPU fallback resolved for one instance of a Pack: the stock element plus
// the property values the adapter sets on it. The sibling of the resolved
// `ShaderPass`/`TransitionPass`, but for the path that runs a built-in filter
// instead of a shader - so the engine can still apply a Pack whose passes the
// GPU path cannot express.
struct ResolvedCpuFallback
{
    // The GStreamer element factory name.
    std::string element;
    // Property values in the manifest's declaration order.
    std::vector<CpuFallbackValue> values;
};

// Resolves the Pack's declared `[cpu]` fallback for one instance into the
// element and property values the adapter runs. Returns nullopt when the Pack
// declares no fallback, or when the instance names parameters the pack does
// not declare. The element's existence is not checked here - the caller owns
// that, so an availability probe (or its absence) decides whether the fallback
// is emitted, never whether it can be formed.
std::optional<ResolvedCpuFallback>
resolve_cpu_fallback(const Pack &pack, const genesis::project::AppliedFilter &instance, double at);

// An audio Pack resolved for one instance of it: the stock GStreamer audio
// element plus the property values the adapter sets on it. The audio-path
// sibling of `ResolvedCpuFallback`; its values reuse `CpuFallbackValue` - the
// same record, an element property at an evaluated value.
struct ResolvedAudio
{
    // The GStreamer element factory name.
    std::string element;
    // Property values in the manifest's declaration order.
    std::vector<CpuFallbackValue> values;
};

// Resolves the Pack's declared `[audio]` backend for one instance into the
// element and property values the adapter runs on a clip's audio path.
// Returns nullopt when the Pack declares no backend, or when the instance
// names parameters the pack does not declare. The element's existence is not
// checked here - the caller owns that, so an availability probe (or its
// absence) decides whether the effect is emitted, never whether it can be
// formed.
std::optional<ResolvedAudio>
resolve_audio(const Pack &pack, const genesis::project::AppliedFilter &instance, double at);

} // namespace genesis::effects
