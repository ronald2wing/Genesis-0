// SPDX-License-Identifier: GPL-3.0-or-later
// SPDX-FileCopyrightText: 2026 Genesis-0 contributors

#pragma once

#include <string>

#include "core/ShaderPass.h"

// GStreamer types are forward-declared so this header stays lean; only the
// adapter .cpp files include the full GStreamer headers (matching
// TextureMix.h). This is an adapter header, not a host header: it may name
// GStreamer types, but it never crosses into genesis_host (R5/R6).
typedef struct _GstElement GstElement;
typedef struct _GstStructure GstStructure;

namespace genesis::adapters::engine::ges {

// Registers the "lutgrade" element with GStreamer once. Idempotent: a second
// call after a successful first is a no-op returning true. Returns false only
// when the element type cannot be registered.
bool register_lut_grade();

// Creates a "lutgrade" element - the GstGLFilter subclass that samples one 2D
// picture texture and one 3D look-up table in a single fragment shader, the
// escape hatch for a colour-look pass's resolved table that stock glshader
// cannot express (glshader binds exactly one 2D texture,
// docs/decisions/effect-execution.md §6).
//
// `fragment` is the FULL fragment source (prelude included), exactly like the
// glshader path's `fragment_source_lut_for` composes it: the source declares
// the `tex` and `lut3d` samplers, the pass's own uniforms (e.g. `strength`),
// and the body that samples `texture3D(lut3d, c.rgb)`. `uniforms` is the same
// GstStructure `ShaderChain.h uniforms_for` builds; its values are uploaded to
// the matching declarations each frame. It may be null for a body with no
// uniforms.
//
// `lut` is the resolved table, uploaded once as a GL_TEXTURE_3D at build time.
// Returns nullptr when the element cannot be registered or created, or when
// the table's data does not match `lut.size` (a table the resolver would have
// refused anyway).
//
// The caller owns the returned element (gst_object_unref).
GstElement *make_lut_grade(const std::string &fragment, GstStructure *uniforms,
                           const genesis::core::Lut &lut);

} // namespace genesis::adapters::engine::ges
