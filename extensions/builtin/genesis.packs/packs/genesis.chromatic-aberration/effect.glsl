// SPDX-License-Identifier: GPL-3.0-or-later
// SPDX-FileCopyrightText: 2026 Genesis-0 contributors
//
// The fragment BODY; see genesis.exposure/effect.glsl for the contract.
// `width` is set by glshader every frame, not by the pack.

uniform float width;

void main () {
    vec4 c = texture2D (tex, v_texcoord);
    float scale = shift / (0.5 * width);
    vec2 from_middle = v_texcoord - vec2 (0.5);
    float red = texture2D (tex, vec2 (0.5) + from_middle / (1.0 + scale)).r;
    float blue = texture2D (tex, vec2 (0.5) + from_middle / max (1.0 - scale, 0.01)).b;
    gl_FragColor = vec4 (red, c.g, blue, c.a);
}
