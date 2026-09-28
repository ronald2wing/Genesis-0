// SPDX-License-Identifier: GPL-3.0-or-later
// SPDX-FileCopyrightText: 2026 Genesis-0 contributors
//
// GLSL fragment bodies for the Phase 4 multi-pass gate spike. Every shader
// declares its own precision/varyings/uniform because glshader injects none
// (docs/dependencies.md "Phase 0 spike result"). No // comments inside: the
// bodies are single-line strings and a // would comment out the rest.

#ifndef GL_MULTIPASS_SHADERS_H
#define GL_MULTIPASS_SHADERS_H

// The one texture glshader binds to `tex`.
#define GL_PRELUDE \
    "precision mediump float; varying vec2 v_texcoord; uniform sampler2D tex; "

// Passes the input through unchanged.
static const char SHADER_IDENTITY[] =
    GL_PRELUDE
    "void main () { gl_FragColor = texture2D (tex, v_texcoord); }";

// Inverts RGB, keeps alpha.
static const char SHADER_INVERT[] =
    GL_PRELUDE
    "void main () { vec4 c = texture2D (tex, v_texcoord);"
    " gl_FragColor = vec4 (1.0 - c.rgb, c.a); }";

// Constant colours, for the fan-out / intermediate tests.
static const char SHADER_RED[] =
    GL_PRELUDE "void main () { gl_FragColor = vec4 (1.0, 0.0, 0.0, 1.0); }";
static const char SHADER_GREEN[] =
    GL_PRELUDE "void main () { gl_FragColor = vec4 (0.0, 1.0, 0.0, 1.0); }";
static const char SHADER_BLUE[] =
    GL_PRELUDE "void main () { gl_FragColor = vec4 (0.0, 0.0, 1.0, 1.0); }";

// Declares a second sampler and reads it, to test whether glshader binds
// anything beyond its single input `tex`. `tex` is left unused on purpose.
static const char SHADER_SECOND_SAMPLER[] =
    "precision mediump float; varying vec2 v_texcoord;"
    " uniform sampler2D tex; uniform sampler2D tex2;"
    " void main () { gl_FragColor = texture2D (tex2, v_texcoord); }";

// Horizontal 5-tap box blur at 320x240 (texel x = 1/320).
static const char SHADER_HBLUR[] =
    GL_PRELUDE
    "void main () { float d = 0.003125;"
    " vec4 s = texture2D (tex, v_texcoord)"
    "  + texture2D (tex, v_texcoord + vec2 (d, 0.0))"
    "  + texture2D (tex, v_texcoord - vec2 (d, 0.0))"
    "  + texture2D (tex, v_texcoord + vec2 (2.0 * d, 0.0))"
    "  + texture2D (tex, v_texcoord - vec2 (2.0 * d, 0.0));"
    " gl_FragColor = s / 5.0; }";

// Vertical 5-tap box blur at 320x240 (texel y = 1/240).
static const char SHADER_VBLUR[] =
    GL_PRELUDE
    "void main () { float d = 0.0041667;"
    " vec4 s = texture2D (tex, v_texcoord)"
    "  + texture2D (tex, v_texcoord + vec2 (0.0, d))"
    "  + texture2D (tex, v_texcoord - vec2 (0.0, d))"
    "  + texture2D (tex, v_texcoord + vec2 (0.0, 2.0 * d))"
    "  + texture2D (tex, v_texcoord - vec2 (0.0, 2.0 * d));"
    " gl_FragColor = s / 5.0; }";

// The same 5x5 box blur folded into one body (25 taps, no intermediate).
static const char SHADER_FOLD_BLUR[] =
    GL_PRELUDE
    "void main () { vec2 t = vec2 (0.003125, 0.0041667); vec4 s = vec4 (0.0);"
    " for (int y = -2; y <= 2; y++) { for (int x = -2; x <= 2; x++) {"
    "  s += texture2D (tex, v_texcoord + vec2 (float (x) * t.x, float (y) * t.y));"
    " } }"
    " gl_FragColor = s / 25.0; }";

#endif  // GL_MULTIPASS_SHADERS_H
