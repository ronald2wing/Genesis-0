// SPDX-License-Identifier: GPL-3.0-or-later
// SPDX-FileCopyrightText: 2026 Genesis-0 contributors

#pragma once

#include <string>

// GStreamer types are forward-declared so this header stays lean; only the
// adapter .cpp files include the full GStreamer headers (matching
// GesBuilder.h). This is an adapter header, not a host header: it may name
// GStreamer types, but it never crosses into genesis_host (R5/R6).
typedef struct _GstElement GstElement;
typedef struct _GstStructure GstStructure;

namespace genesis::adapters::engine::ges {

// Registers the "texturemix" element with GStreamer once. Idempotent: a second
// call after a successful first is a no-op returning true. Returns false only
// when the element type cannot be registered.
bool register_texture_mix();

// Creates a "texturemix" element - the GstGLMixer subclass that binds N input
// textures into one fragment shader, each on its own texture unit with an
// explicit glUniform1i (the escape hatch for named intermediates and a title's
// per-word reveal, which stock glshader cannot express -
// docs/decisions/effect-execution.md §6).
//
// `fragment` is the GLSL body only: the element owns the prelude (precision,
// the varying, and the tex0..tex7 samplers), so a body references tex0, tex1,
// ... and declares its own non-sampler uniforms. `uniforms` is the same
// GstStructure the shader chain builds (ShaderChain.h uniforms_for); its values
// are uploaded to the matching declarations each frame. It may be null for a
// body with no uniforms.
//
// The caller owns the returned element (gst_object_unref). Returns nullptr when
// the element cannot be registered or created.
GstElement *make_texture_mix(const std::string &fragment, GstStructure *uniforms);

} // namespace genesis::adapters::engine::ges
