// SPDX-License-Identifier: GPL-3.0-or-later
// SPDX-FileCopyrightText: 2026 Genesis-0 contributors

#pragma once

#include <cstdint>
#include <functional>
#include <vector>

#include "core/ShaderPass.h"
#include "effects/Resolve.h"
#include "project/model/Clip.h"
#include "render/Catalogue.h"

// GES types are forward-declared so this header stays lean; only the adapter
// .cpp files include the full GES headers (matching ShaderChain.h).
typedef struct _GESClip GESClip;
typedef struct _GESEffect GESEffect;
typedef struct _GstElement GstElement;

namespace genesis::adapters::engine::ges {

// A clip's per-pass effect chain: one GES top effect per expressible pass,
// installed once and driven per buffer by a uniform probe. Mirrors the
// treatment path's static-graph / re-upload design
// (docs/decisions/effect-animation.md §3): the graph is built once, a keyed
// knob rides by re-resolving the clip's filters each buffer and uploading only
// the uniforms that changed.
//
// The probe's buffer PTS is source media time (the top effects sit between the
// source and the rest of the pipeline, before GES restamps timestamps), so the
// clip-local fraction is (pts - source_start_ns) / duration_ns - see
// clip_fraction. This differs from a treatment, whose effect clip reads
// absolute timeline time.
struct ClipEffectChain
{
    // One top effect per expressible pass, in caller order (the first pass is
    // applied first). The effect is owned by the clip and the shader by the
    // effect's bin; both are borrowed here and never unreffed.
    struct Slot
    {
        GESEffect *effect = nullptr;
        GstElement *shader = nullptr;
    };
    std::vector<Slot> slots;
    // A CPU fallback's top effect and the property values resolved for it,
    // applied after add_track by configure_clip_effects (the stock element's
    // properties register as child props only once the bin materialises).
    struct CpuSlot
    {
        GESEffect *effect = nullptr;
        std::vector<genesis::effects::CpuFallbackValue> values;
    };
    std::vector<CpuSlot> cpu_slots;
    // The passes last uploaded, so a frame whose knobs read the same uploads
    // nothing (the allocation-free ShaderPass::operator== compare).
    std::vector<genesis::core::ShaderPass> last;
    // The clip's effects, copied, so each frame resolves against the catalogue
    // without touching the UI's model.
    genesis::project::Clip clip;
    // Where the clip starts reading its source, and how long it runs, both in
    // nanoseconds. From the host clip's source_start and duration.
    std::int64_t source_start_ns = 0;
    std::int64_t duration_ns = 0;
    // The catalogue resolving `clip` each frame; null means the builtin.
    const genesis::render::Catalogue *catalogue = nullptr;
    // The element-availability probe the build consults for a CPU fallback.
    // Empty means the host's own registry (gst_element_factory_find) answers;
    // a caller may inject one (tests) to force a fallback to degrade.
    std::function<bool(const std::string &)> available;
    // Whether the clip's enabled effects carry any keyed (animated) parameter.
    // Set by install_clip_effects: a static chain resolves identically at every
    // fraction, so update_clip_uniforms short-circuits to the prior uniforms
    // instead of re-resolving on the streaming thread. Defaults true so a chain
    // assembled without install_clip_effects (the resolve-only tests) keeps
    // resolving until proven static.
    bool is_animated = true;
};

// Resolves the chain's effects at build time and installs one GES top effect
// per expressible pass that can run as one glshader, plus one top effect per
// CPU fallback (a link the GPU path cannot run but whose pack declares a
// `[cpu]` element), in caller order. Must run before add_track: the top
// effects must land in the clip before its track element is created. Fills
// `chain->slots` and `chain->cpu_slots`, and seeds `chain->last` with the
// build-time GPU resolution; the shaders stay null until
// configure_clip_effects.
//
// Returns true when at least one pass or fallback installed, false when none
// could - the caller then knows the clip carries no renderable effect. A pass
// the adapter cannot express (a curve's vec4[8] field, a second sampler) is
// skipped, not fatal, matching the R9 fallback for a clip effect.
bool install_clip_effects(GESClip *clip, ClipEffectChain *chain);

// After add_track: finds each top effect's glshader, sets its fragment and
// build-time uniforms, sets each CPU fallback's element properties (via
// g_object_set, laying each resolved double on the element's own double
// property), and attaches the per-buffer uniform probe to the first
// shader's sink pad. On success the chain is owned by the probe (its destroy
// notify frees it when the glshader's pad is disposed, at teardown); on
// failure the caller keeps ownership. A CPU-only chain has no shader to probe,
// so its properties are applied but it returns false - the caller frees the
// chain and the clip keeps its static CPU effects.
bool configure_clip_effects(GESClip *clip, ClipEffectChain *chain);

// The clip-local fraction the buffer at `pts_ns` (source media time, as the
// top effects see it) falls at, clamped to 0..1: how far through the clip the
// frame is, which is what each keyed knob's `at` measures.
double clip_fraction(const ClipEffectChain &chain, std::int64_t pts_ns);

// Re-resolves the chain's effects at `fraction` (0..1 of the clip) and uploads
// each changed pass's uniforms to its glshader. Returns whether anything
// changed. A chain whose enabled effects carry no keyed parameter is static:
// it resolves identically at every fraction, so this short-circuits to the
// prior uniforms (returning false) without re-resolving. A resolution that
// disagrees with the built chain in pass count or in a pass's shader/layout is
// a structural change a probe cannot rebuild, so nothing is uploaded: the
// installed glshaders keep their last valid uniforms and the mismatch is
// warned once. Positional upload would pour one pass's uniforms into another's
// shader, so it is never attempted.
bool update_clip_uniforms(ClipEffectChain &chain, double fraction);

} // namespace genesis::adapters::engine::ges
