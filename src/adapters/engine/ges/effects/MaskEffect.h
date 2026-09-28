// SPDX-License-Identifier: GPL-3.0-or-later
// SPDX-FileCopyrightText: 2026 Genesis-0 contributors

#pragma once

#include "ai/vision/MaskStore.h"
#include "core/ClipTimeline.h"

// GES types are forward-declared so this header stays lean; only the adapter
// .cpp files include the full GES headers (matching RevealEffect.h).
typedef struct _GESBaseEffect GESBaseEffect;
typedef struct _GESClip GESClip;

namespace genesis::adapters::engine::ges {

// The alpha-modulation body the mask effect samples: the picture's colour, and
// its alpha multiplied by the mask's single-channel value, so the mask cuts
// the picture without repainting its colour. In the texturemix convention: tex0
// is the picture, tex1 the mask (its `.r` channel), and the body declares no
// non-sampler uniforms. The colour is scaled by the same factor as the alpha
// (premultiplied), so a cut pixel is transparent black (0,0,0,0) - matching the
// reveal effect's hidden region and what the downstream compositor expects of a
// premultiplied input - rather than a colour-bearing but invisible pixel.
inline constexpr const char *kMaskBody = "void main () { vec4 c = texture2D (tex0, v_texcoord);"
                                         " float a = texture2D (tex1, v_texcoord).r;"
                                         " gl_FragColor = vec4 (c.rgb * a, c.a * a); }";

// Registers the mask-effect GType with GES. Idempotent: a second call is a
// no-op returning true. Returns false only when the type is not a
// GESBaseEffect subclass (a programming error that cannot recover).
bool register_mask_effect();

// Builds and configures a mask effect - a custom GESBaseEffect whose element is
// the texturemix bin that samples a clip's picture (tex0) against a per-frame
// mask (tex1) in one fragment shader. Unlike the reveal effect, the mask is
// per-frame, not static: a probe on the picture sink pad pushes the mask frame
// matching each picture buffer's source time into the mask appsrc (no
// imagefreeze), and a missing mask degrades to an opaque frame so the clip
// renders whole rather than black.
//
// `spec` is the mask facts the flattener carried (root, source, subject, model,
// size, stride, last frame); `rate` is the timeline frame rate, used to turn a
// picture buffer's source-time PTS into the source frame index the masks are
// keyed by. The mask is selected by the decimation mapping
// `floor(source_frame / stride)` (MaskStore::mask_frame_for), clamped to the
// last generated frame when the spec records one.
//
// Returns a floating GESBaseEffect*: the caller sinks it via
// install_mask_effect (or g_object_ref_sink) before add_track. Returns nullptr
// when the spec's size is zero (a mask that could never be read).
GESBaseEffect *make_mask_effect(const genesis::ai::vision::MaskSpec &spec,
                                genesis::core::FrameRate rate);

// Adds `effect` as a top effect on `clip`. Mirrors install_reveal_effect's
// ownership: sinks the floating reference, adds at the top, then drops the
// local reference, so both of ges_clip_add_top_effect's failure paths keep the
// effect alive until the clip takes its own reference. Must run before
// add_track. Returns whether the effect landed.
bool install_mask_effect(GESClip *clip, GESBaseEffect *effect);

} // namespace genesis::adapters::engine::ges
