// SPDX-License-Identifier: GPL-3.0-or-later
// SPDX-FileCopyrightText: 2026 Genesis-0 contributors

#pragma once

#include "core/ShaderPass.h"

// GES types are forward-declared so this header stays lean; only the adapter
// .cpp files include the full GES headers (matching ClipEffects.h).
typedef struct _GESBaseEffect GESBaseEffect;
typedef struct _GESClip GESClip;

namespace genesis::adapters::engine::ges {

// The filter id a later stage opts a title into the reveal path with. The
// reveal effect is never installed implicitly: a pass carrying no reveal map
// stays on the glshader path, so this id is the explicit opt-in marker.
inline constexpr const char *kRevealFilterId = "genesis.reveal";

// The production reveal fragment body, in the texturemix convention: tex0 is
// the title's picture, tex1 its baked per-word order map, and a pixel is kept
// only where its word's order has reached `progress`. The builder supplies this
// as the pass source (the resolver never attaches a reveal map, so it never
// produces a reveal body); byte-for-byte the body the adapter tests compile.
inline constexpr const char *kRevealBody =
        "uniform float progress;"
        " void main () { vec4 c = texture2D (tex0, v_texcoord);"
        " float order = texture2D (tex1, v_texcoord).r;"
        " gl_FragColor = (order <= progress) ? c : vec4 (0.0, 0.0, 0.0, 0.0); }";

// Registers the reveal-effect GType with GES. Idempotent: a second call is a
// no-op returning true. Returns false only when the type is not a
// GESBaseEffect subclass (a programming error that cannot recover).
bool register_reveal_effect();

// Builds and configures a reveal effect - a custom GESBaseEffect whose element
// is the texturemix bin that samples a title's picture (tex0) against its
// baked reveal-order map (tex1) in one fragment shader, which stock glshader
// cannot express (docs/decisions/effect-execution.md §6). The pass's
// `reveal_map` must be present; the picture chain, the GRAY8 -> RGBA GLMemory
// map chain, and the GLMemory -> system-memory output chain are all built
// here, so the effect needs no GESEffect auto videoconvert wrapper.
//
// Returns a floating GESBaseEffect*: the caller sinks it via
// install_reveal_effect (or g_object_ref_sink) before add_track. Returns
// nullptr when the pass carries no reveal map or the asset cannot be
// requested/extracted.
GESBaseEffect *make_reveal_effect(const genesis::core::ShaderPass &pass);

// Adds `effect` as a top effect on `clip`. Mirrors install_clip_effects'
// ownership: sinks the floating reference, adds at the top, then drops the
// local reference, so both of ges_clip_add_top_effect's failure paths keep the
// effect alive until the clip takes its own reference. Must run before
// add_track. Returns whether the effect landed.
bool install_reveal_effect(GESClip *clip, GESBaseEffect *effect);

// Rewrites the `progress` uniform of the effect's texturemix to `progress`
// (clamped by the caller). Runs on the streaming thread: it only rewrites the
// pass copy it owns and g_object_sets the mix it already holds, touching
// nothing the UI owns. Returns false when the effect has no mix or the pass
// carries no `progress` field.
bool update_reveal_progress(GESBaseEffect *effect, double progress);

} // namespace genesis::adapters::engine::ges
