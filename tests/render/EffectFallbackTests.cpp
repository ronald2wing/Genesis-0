// SPDX-License-Identifier: GPL-3.0-or-later
// SPDX-FileCopyrightText: 2026 Genesis-0 contributors

#include <cstdio>
#include <functional>
#include <map>
#include <optional>
#include <string>
#include <utility>
#include <vector>

#include "../support/Checks.h"
#include "effects/Pack.h"
#include "project/model/Clip.h"
#include "project/model/Effects.h"
#include "render/EffectFallback.h"

namespace {

using genesis::effects::CpuFallback;
using genesis::effects::Kind;
using genesis::effects::Pack;
using genesis::effects::Parameter;
using genesis::effects::Pass;
using genesis::project::AppliedFilter;
using genesis::project::Clip;
using genesis::render::ChainResolution;
using genesis::render::resolve_effect_chain;
using genesis::test::check;

// A single-slider effect Pack, expressible: no passes before `effect`.
Pack effect_pack(std::string id)
{
    Parameter amount;
    amount.key = "amount";
    amount.label = "Amount";
    amount.min = 0.0;
    amount.max = 1.0;
    amount.default_value = 0.5;

    Pack pack;
    pack.id = std::move(id);
    pack.name = "Amount";
    pack.kind = Kind::Effect;
    pack.parameters = { amount };
    return pack;
}

// The same Pack with a named-intermediate pass: not expressible on the
// glshader path (docs/decisions/effect-execution.md §6).
Pack inexpressible_pack(std::string id)
{
    Pack pack = effect_pack(std::move(id));
    Pass blur;
    blur.target = "blur";
    pack.passes = { blur };
    return pack;
}

// An inexpressible pack that declares a CPU fallback onto `videobalance`.
Pack cpu_pack(std::string id)
{
    Pack pack = inexpressible_pack(std::move(id));
    CpuFallback cpu;
    cpu.element = "videobalance";
    cpu.properties = { { "brightness", "amount" } };
    pack.cpu = cpu;
    return pack;
}

// A catalogue as the injected lookup, backed by an in-process map. The lambda
// borrows the map and returns a pointer into it - the catalogue's Pack lives
// as long as the map, which outlives the resolve call in each test.
std::function<const Pack *(const std::string &)>
lookup_from(const std::map<std::string, Pack> &catalogue)
{
    return [&catalogue](const std::string &id) -> const Pack * {
        const auto it = catalogue.find(id);
        if (it == catalogue.end()) {
            return nullptr;
        }
        return &it->second;
    };
}

// A clip whose effect chain is `effects`.
Clip clip_with(std::string id, std::vector<AppliedFilter> effects)
{
    Clip clip;
    clip.id = std::move(id);
    clip.video_effects = std::move(effects);
    return clip;
}

void test_an_unknown_pack_is_skipped_and_the_rest_resolves()
{
    const std::map<std::string, Pack> catalogue{
        { "example.glow", effect_pack("example.glow") },
    };
    Clip clip = clip_with("c1",
                          {
                                  AppliedFilter::create("example.glow"),
                                  AppliedFilter::create("example.missing"),
                          });
    const Clip before = clip;

    const ChainResolution result = resolve_effect_chain(clip, 0.0, lookup_from(catalogue));

    check(result.passes.size() == 1, "the resolvable effect resolves");
    check(result.skips.size() == 1, "one link is skipped");
    check(result.skips[0].clip_id == "c1", "the skip names the clip");
    check(result.skips[0].pack_id == "example.missing", "the skip names the unknown pack");
    check(result.skips[0].reason == "unknown pack", "the unknown reason");
    check(result.passes[0].package == "example.glow", "the resolved pass is the known effect");
    check(clip == before, "the clip is unchanged");
}

void test_an_inexpressible_pack_is_skipped()
{
    const std::map<std::string, Pack> catalogue{
        { "example.gaussian-blur", inexpressible_pack("example.gaussian-blur") },
    };
    Clip clip = clip_with("c1",
                          {
                                  AppliedFilter::create("example.gaussian-blur"),
                          });

    const ChainResolution result = resolve_effect_chain(clip, 0.0, lookup_from(catalogue));

    check(result.passes.empty(), "nothing resolves");
    check(result.skips.size() == 1, "the inexpressible pack is skipped");
    check(result.skips[0].reason == "not expressible", "the not-expressible reason");
    check(result.skips[0].pack_id == "example.gaussian-blur",
          "the skip names the inexpressible pack");
}

void test_a_fully_skippable_chain_is_not_a_failure()
{
    const std::map<std::string, Pack> catalogue{
        { "example.gaussian-blur", inexpressible_pack("example.gaussian-blur") },
    };
    Clip clip = clip_with("c1",
                          {
                                  AppliedFilter::create("example.missing"),
                                  AppliedFilter::create("example.gaussian-blur"),
                          });

    const ChainResolution result = resolve_effect_chain(clip, 0.0, lookup_from(catalogue));

    check(result.passes.empty(), "no pass resolves");
    check(result.skips.size() == 2, "every link is recorded, not fatal");
    check(result.skips[0].pack_id == "example.missing" && result.skips[0].reason == "unknown pack",
          "the unknown link is recorded first");
    check(result.skips[1].pack_id == "example.gaussian-blur"
                  && result.skips[1].reason == "not expressible",
          "the inexpressible link is recorded second");
}

void test_an_undeclared_parameter_is_skipped_while_the_rest_resolves()
{
    const std::map<std::string, Pack> catalogue{
        { "example.glow", effect_pack("example.glow") },
    };
    AppliedFilter stray = AppliedFilter::create("example.glow");
    stray.params["bogus"] = 3.0;
    Clip clip = clip_with("c1",
                          {
                                  AppliedFilter::create("example.glow"),
                                  stray,
                          });

    const ChainResolution result = resolve_effect_chain(clip, 0.0, lookup_from(catalogue));

    check(result.passes.size() == 1, "the clean link still resolves");
    check(result.skips.size() == 1, "the stray instance is skipped");
    check(result.skips[0].reason == "bad parameters", "the bad-parameters reason");
    check(result.skips[0].pack_id == "example.glow", "the stray instance names its pack");
}

void test_resolution_never_mutates_the_clip()
{
    const std::map<std::string, Pack> catalogue{
        { "example.glow", effect_pack("example.glow") },
        { "example.gaussian-blur", inexpressible_pack("example.gaussian-blur") },
    };
    Clip clip = clip_with("c1",
                          {
                                  AppliedFilter::create("example.glow"),
                                  AppliedFilter::create("example.missing"),
                                  AppliedFilter::create("example.gaussian-blur"),
                          });
    const Clip before = clip;

    const ChainResolution result = resolve_effect_chain(clip, 0.5, lookup_from(catalogue));

    check(clip == before, "R9: the clip is byte-identical after the call");
    check(result.passes.size() == 1, "the one expressible effect resolves");
    check(result.skips.size() == 2, "the two unsupported links are recorded");
}

void test_a_cpu_fallback_is_emitted_when_the_element_is_available()
{
    const std::map<std::string, Pack> catalogue{
        { "example.video-balance", cpu_pack("example.video-balance") },
    };
    Clip clip = clip_with("c1",
                          {
                                  AppliedFilter::create("example.video-balance"),
                          });

    const ChainResolution result =
            resolve_effect_chain(clip, 0.0, lookup_from(catalogue), [](const std::string &element) {
                return element == "videobalance";
            });

    check(result.passes.empty(), "a cpu-only pack yields no shader pass");
    check(result.skips.empty(), "the link is not skipped");
    check(result.cpu.size() == 1, "one cpu fallback is emitted");
    check(result.cpu[0].element == "videobalance", "the element name");
    check(result.cpu[0].values.size() == 1 && result.cpu[0].values[0].key == "brightness"
                  && result.cpu[0].values[0].value == 0.5,
          "the property maps the parameter's default");
}

void test_a_cpu_fallback_is_skipped_when_the_element_is_absent()
{
    const std::map<std::string, Pack> catalogue{
        { "example.video-balance", cpu_pack("example.video-balance") },
    };
    Clip clip = clip_with("c1",
                          {
                                  AppliedFilter::create("example.video-balance"),
                          });

    const ChainResolution result = resolve_effect_chain(clip, 0.0, lookup_from(catalogue),
                                                        [](const std::string &) { return false; });

    check(result.cpu.empty(), "an absent element emits no fallback");
    check(result.passes.empty(), "and no shader pass");
    check(result.skips.size() == 1, "the link is skipped");
    check(result.skips[0].reason == "not expressible", "the reason");
}

void test_a_cpu_fallback_is_skipped_without_a_probe()
{
    const std::map<std::string, Pack> catalogue{
        { "example.video-balance", cpu_pack("example.video-balance") },
    };
    Clip clip = clip_with("c1",
                          {
                                  AppliedFilter::create("example.video-balance"),
                          });

    const ChainResolution result = resolve_effect_chain(clip, 0.0, lookup_from(catalogue));

    check(result.cpu.empty(), "no probe means no fallback is emitted");
    check(result.skips.size() == 1, "the link keeps the engine-free skip");
    check(result.skips[0].reason == "not expressible", "the reason");
}

void test_an_inexpressible_pack_without_a_fallback_is_skipped_even_with_a_probe()
{
    const std::map<std::string, Pack> catalogue{
        { "example.gaussian-blur", inexpressible_pack("example.gaussian-blur") },
    };
    Clip clip = clip_with("c1",
                          {
                                  AppliedFilter::create("example.gaussian-blur"),
                          });

    const ChainResolution result = resolve_effect_chain(clip, 0.0, lookup_from(catalogue),
                                                        [](const std::string &) { return true; });

    check(result.cpu.empty(), "a pack with no fallback emits none");
    check(result.skips.size() == 1, "the link is skipped");
    check(result.skips[0].reason == "not expressible", "the reason");
}

} // namespace

int main()
{
    test_an_unknown_pack_is_skipped_and_the_rest_resolves();
    test_an_inexpressible_pack_is_skipped();
    test_a_fully_skippable_chain_is_not_a_failure();
    test_an_undeclared_parameter_is_skipped_while_the_rest_resolves();
    test_resolution_never_mutates_the_clip();
    test_a_cpu_fallback_is_emitted_when_the_element_is_available();
    test_a_cpu_fallback_is_skipped_when_the_element_is_absent();
    test_a_cpu_fallback_is_skipped_without_a_probe();
    test_an_inexpressible_pack_without_a_fallback_is_skipped_even_with_a_probe();

    return genesis::test::summary();
}
