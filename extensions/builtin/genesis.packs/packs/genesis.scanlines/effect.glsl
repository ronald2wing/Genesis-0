// SPDX-License-Identifier: GPL-3.0-or-later
// SPDX-FileCopyrightText: 2026 Genesis-0 contributors
//
// The fragment BODY; see genesis.exposure/effect.glsl for the contract.
// `height` is set by glshader every frame, not by the pack.

uniform float height;

void main () {
    vec4 c = texture2D (tex, v_texcoord);
    float row = fract (v_texcoord.y * height / 3.0);
    float dark = 1.0 - strength / 100.0 * step (0.5, row) * 0.5;
    gl_FragColor = vec4 (c.rgb * dark, c.a);
}
