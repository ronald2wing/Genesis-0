// SPDX-License-Identifier: GPL-3.0-or-later
// SPDX-FileCopyrightText: 2026 Genesis-0 contributors

#pragma once

#include <optional>
#include <string>
#include <utility>
#include <vector>

#include "core/ShaderPass.h"
#include "render/Resolve.h"

// GStreamer types are forward-declared so this header stays lean; only the
// adapter .cpp files include the full GStreamer headers (matching GesBuilder.h).
// GstPadProbeReturn is an anonymous C enum (gst/gstpad.h), so the probe
// callback cannot be declared here by type; `attach_treatment_probe` wraps it
// behind a bool so the enum never has to cross this header.
typedef struct _GstElement GstElement;
typedef struct _GstStructure GstStructure;

namespace genesis::adapters::engine::ges {

// Builds the fragment source one glshader runs: the prelude glshader does not
// inject, the pass's own uniform declarations, and its body. The prototype
// settled that glshader injects nothing and binds exactly one texture
// (docs/decisions/effect-execution.md §6), so every declaration the body
// relies on is emitted here and nothing may declare a second sampler.
//
// Returns nullopt when the pass cannot be expressed as one glshader fragment:
// a body that declares another `sampler2D` is refused rather than emitted,
// because a second sampler silently aliases texture unit 0 with no error
// (§6) and the pass would read its own input again instead of what it asked
// for. A silent hazard is worse than a changed signature, so the return is an
// optional rather than a bare string.
//
// The uniform declarations follow the prelude, one `uniform <type> <name>;`
// per ParamField in `pass.fields`, using the field's GLSL type spelling and
// name. `fields` is produced by the resolver's single walk that also lays out
// `params` (src/effects/Resolve.cpp `params_block`), so the declarations
// emitted here and the values `append_shader_chain` uploads can never disagree
// about a field's name, type, or position.
std::optional<std::string> fragment_source_for(const genesis::core::ShaderPass &pass);

// Builds the fragment source the lutgrade element runs for a pass carrying a
// resolved 3D LUT (`pass.lut` set): the ES-1.00 prelude the element compiles
// verbatim (the `#extension GL_OES_texture_3D` directive, the mediump
// precisions including `sampler3D`, and the `tex`/`lut3d` samplers), the
// pass's own uniform declarations, and a fixed body that samples the table
// `texture3D (lut3d, c.rgb)` and mixes it into the picture by
// `strength / 100.0` - parity with the pack's GLSL fallback, which also mixes
// by the declared `strength` knob. The `lut` enum field stays declared but is
// not read: the table the element binds was already selected by the resolver.
//
// Returns nullopt when the pass cannot run as one lutgrade fragment: it still
// carries pre-stages, it declares a field the `uniforms` structure cannot
// upload (a curve's vec4[8]), or it has no `strength` field (the body's mix
// would name an undeclared uniform). Only the shipped LUT pack
// (genesis.lut-grade) declares `strength`, so this is the documented seam.
std::optional<std::string> fragment_source_lut_for(const genesis::core::ShaderPass &pass);

// Appends the glshader chain for `passes` to `tail`, linking each pass in
// order: one element per pass, uniform declarations in the source and the
// values uploaded through glshader's `uniforms` structure, laid out to match
// `params`. A pass whose source is refused (a second sampler), or that
// declares pre-stages (the named-intermediate model, not expressible),
// aborts the whole chain - a partially built chain would render something
// that is not the effect. Returns the new tail, or nullptr with nothing
// linked on failure.
GstElement *append_shader_chain(GstElement *tail,
                                const std::vector<genesis::core::ShaderPass> &passes);

// Whether a pass can run as one glshader: no pre-stages, every field's value
// uploadable through glshader's `uniforms` (no curve's vec4[8]), and a fragment
// source `fragment_source_for` accepts (no second sampler). Mirrors the
// all-or-nothing refusal `append_shader_chain` applies, so the GES builder can
// refuse a treatment with an inexpressible pass before creating any element.
bool pass_is_expressible(const genesis::core::ShaderPass &pass);

// The `uniforms` GstStructure for one pass: one entry per ParamField, the value
// read from `pass.params` at the field's offset and stored under the field's
// name, so it lands on the declaration `fragment_source_for` emitted for the
// same field. Scalar floats go as G_TYPE_FLOAT; vec2/3/4 as graphene boxed
// values - the only forms glshader's `_set_uniform` understands beyond a
// scalar. The caller owns the returned structure (gst_structure_free).
GstStructure *uniforms_for(const genesis::core::ShaderPass &pass);

// A treatment's glshader chain and the passes last applied to it: what the
// buffer probe drives per frame. The chain is installed once at build time;
// the treatment's keyed knobs then ride by re-resolving and re-uploading only
// the uniforms that changed.
struct TreatmentChain
{
    // `Treatment` has no default constructor (its Rational times must be set),
    // so the chain is built around one; `shaders` and `last` default to empty.
    explicit TreatmentChain(genesis::render::Treatment treatment) : treatment(std::move(treatment))
    {
    }

    // One glshader per resolved pass, in order, owned by the treatment's
    // effect bin - borrowed, never unreffed here.
    std::vector<GstElement *> shaders;
    // The passes last applied to `shaders`, so a frame whose resolution is
    // unchanged uploads nothing.
    std::vector<genesis::core::ShaderPass> last;
    // The treatment whose `passes_at(time)` resolves the chain at each frame.
    // Copied in from `BuiltTimeline::treatments`; its bare catalogue pointer is
    // null on the GES path, so `passes_at` consults the builtin catalogue.
    genesis::render::Treatment treatment;
};

// Re-resolves `chain.treatment.passes_at(time)` and, when the passes differ
// from `chain.last`, uploads each pass's uniforms to its glshader. Returns
// whether the passes changed. The comparison (`ShaderPass::operator==`) is
// allocation-free, so a frame whose knobs have not moved costs nothing beyond
// the re-resolve itself; the re-resolve allocates a pass vector per call, which
// is the documented cost of riding keyed knobs
// (docs/decisions/effect-animation.md §5). Reading the pack body no longer
// happens per call - the catalogue caches bodies - so the remaining per-buffer
// cost is the allocation, not the I/O. A pass-count change between the
// built chain and a later resolution is a structural change a probe cannot
// rebuild, so only the shared prefix is uploaded and the rest is skipped.
bool update_uniforms(TreatmentChain &chain, genesis::core::Rational time);

// Attaches the treatment uniform probe to `shader`'s sink pad: each buffer's
// PTS resolves the treatment at that time and uploads uniforms when they
// changed. `destroy` releases `chain` when the pad's element is disposed - the
// notify passed straight through to gst_pad_add_probe. Returns true when the
// probe attached. The probe runs on the streaming thread, so it must not touch
// anything the UI owns; it only re-resolves the treatment and g_object_sets the
// glshader it already holds.
bool attach_treatment_probe(GstElement *shader, TreatmentChain *chain, void (*destroy)(void *));

} // namespace genesis::adapters::engine::ges
