// SPDX-License-Identifier: GPL-3.0-or-later
// SPDX-FileCopyrightText: 2026 Genesis-0 contributors

#include "render/EffectFallback.h"

#include <string_view>
#include <utility>

#include "effects/Resolve.h"

namespace genesis::render {

namespace {

// The skip reasons, one per cause, so a skipped link says what happened
// without the caller guessing which check failed.
constexpr std::string_view REASON_UNKNOWN = "unknown pack";
constexpr std::string_view REASON_NOT_EXPRESSIBLE = "not expressible";
constexpr std::string_view REASON_BAD_PARAMS = "bad parameters";

} // namespace

ChainResolution resolve_effect_chain(
        const genesis::project::Clip &clip, double at,
        const std::function<const genesis::effects::Pack *(const std::string &)> &lookup,
        const std::function<bool(const std::string &)> &available)
{
    ChainResolution result;
    for (const genesis::project::AppliedFilter &filter : clip.video_effects) {
        const genesis::effects::Pack *pack = lookup(filter.id);
        if (pack == nullptr) {
            result.skips.push_back(EffectSkip{ clip.id, filter.id, std::string(REASON_UNKNOWN) });
            continue;
        }
        if (!genesis::effects::is_expressible(*pack)) {
            // A Pack the GPU path cannot run may still declare a CPU fallback.
            // The caller's `available` probe is the only thing that knows a
            // host's elements: without it (the engine-free callers) the R9
            // skip stands, and with it the fallback is emitted only when the
            // element is present.
            if (available) {
                std::optional<genesis::effects::ResolvedCpuFallback> fallback =
                        genesis::effects::resolve_cpu_fallback(*pack, filter, at);
                if (fallback && available(fallback->element)) {
                    result.cpu.push_back(std::move(*fallback));
                    continue;
                }
            }
            result.skips.push_back(
                    EffectSkip{ clip.id, filter.id, std::string(REASON_NOT_EXPRESSIBLE) });
            continue;
        }
        std::optional<genesis::core::ShaderPass> pass =
                genesis::effects::resolve_pass(*pack, filter, at);
        if (!pass) {
            result.skips.push_back(
                    EffectSkip{ clip.id, filter.id, std::string(REASON_BAD_PARAMS) });
            continue;
        }
        result.passes.push_back(std::move(*pass));
    }
    return result;
}

} // namespace genesis::render
