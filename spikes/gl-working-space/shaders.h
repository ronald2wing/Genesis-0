// SPDX-License-Identifier: GPL-3.0-or-later
// SPDX-FileCopyrightText: 2026 Genesis-0 contributors
//
// GLSL fragment bodies for the working-space spike. Stock glshader injects
// nothing (docs/dependencies.md "Phase 0 spike result"), so each body
// declares its own precision/varying/sampler. No // comments inside any
// body: each is a single-line string and a // would comment out the rest.

#ifndef GL_WORKING_SPACE_SHADERS_H
#define GL_WORKING_SPACE_SHADERS_H

#define GL_PRELUDE \
    "precision mediump float; varying vec2 v_texcoord; uniform sampler2D tex; "

// Passes the single input through unchanged (used on the main branch so the
// Q2 frame-rate probes carry a real effect pass, not a bare tee).
static const char SHADER_IDENTITY[] =
    GL_PRELUDE
    "void main () { gl_FragColor = texture2D (tex, v_texcoord); }";

#endif  // GL_WORKING_SPACE_SHADERS_H
