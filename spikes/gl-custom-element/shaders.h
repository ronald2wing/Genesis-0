// SPDX-License-Identifier: GPL-3.0-or-later
// SPDX-FileCopyrightText: 2026 Genesis-0 contributors
//
// GLSL bodies for the custom-element spike (spikes/gl-custom-element).
// The stock glshader shaders (RED/GREEN/INVERT) follow the Phase 0/4
// convention: glshader injects nothing, so each declares its own precision,
// varying, and `sampler2D tex`. The gltexmix shaders declare the samplers
// the element binds to distinct texture units (tex0, tex1, ...) — the element
// itself injects the vertex stage (gltexmix.c VERTEX_SRC).
//
// No // comments inside any body: each is a single-line string and a //
// would comment out the remainder.

#ifndef GL_CUSTOM_ELEMENT_SHADERS_H
#define GL_CUSTOM_ELEMENT_SHADERS_H

/* --- stock glshader shaders (single `tex` input) ---------------------- */

#define GL_PRELUDE \
    "precision mediump float; varying vec2 v_texcoord; uniform sampler2D tex; "

static const char SHADER_RED[] =
    GL_PRELUDE "void main () { gl_FragColor = vec4 (1.0, 0.0, 0.0, 1.0); }";
static const char SHADER_GREEN[] =
    GL_PRELUDE "void main () { gl_FragColor = vec4 (0.0, 1.0, 0.0, 1.0); }";
static const char SHADER_INVERT[] =
    GL_PRELUDE "void main () { vec4 c = texture2D (tex, v_texcoord);"
    " gl_FragColor = vec4 (1.0 - c.rgb, c.a); }";

/* --- gltexmix two-input shaders (tex0 + tex1) ------------------------- */

/* Decisive combine test: R channel from tex0, G channel from tex1. With
 * tex0=RED and tex1=GREEN this yields YELLOW only if the two samplers are
 * bound to distinct textures. If tex1 silently aliased tex0 (the glshader
 * failure), the result would be RED (1,0,0). */
static const char FRAG_YELLOW[] =
    "precision mediump float; varying vec2 v_texcoord;"
    " uniform sampler2D tex0; uniform sampler2D tex1;"
    " void main () { vec4 a = texture2D (tex0, v_texcoord);"
    " vec4 b = texture2D (tex1, v_texcoord);"
    " gl_FragColor = vec4 (a.r, b.g, 0.0, 1.0); }";

/* Anti-alias control: sample ONLY tex1 and ignore tex0. With tex0=RED and
 * tex1=GREEN the output is GREEN if tex1 is independently bound, RED if it
 * aliases tex0. */
static const char FRAG_TEX1[] =
    "precision mediump float; varying vec2 v_texcoord;"
    " uniform sampler2D tex0; uniform sampler2D tex1;"
    " void main () { gl_FragColor = texture2D (tex1, v_texcoord); }";

/* Named-intermediate combine: average tex0 (the earlier pass's output) with
 * tex1 (the original layer). With tex0 = invert(original), the output is
 * 0.5 * (original + (1 - original)) = constant 0.5 gray — only reachable if
 * both inputs are sampled. */
static const char FRAG_AVG[] =
    "precision mediump float; varying vec2 v_texcoord;"
    " uniform sampler2D tex0; uniform sampler2D tex1;"
    " void main () { gl_FragColor = 0.5 * texture2D (tex0, v_texcoord)"
    "  + 0.5 * texture2D (tex1, v_texcoord); }";

#endif  // GL_CUSTOM_ELEMENT_SHADERS_H
