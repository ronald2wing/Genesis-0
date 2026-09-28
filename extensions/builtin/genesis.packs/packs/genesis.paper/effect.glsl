// SPDX-License-Identifier: GPL-3.0-or-later
// SPDX-FileCopyrightText: 2026 Genesis-0 contributors
//
// The fragment BODY; see genesis.exposure/effect.glsl for the contract.

uniform float width;
uniform float height;

float hash (vec2 p) {
    return fract (sin (dot (p, vec2 (127.1, 311.7))) * 43758.5453123);
}

void main () {
    vec4 c = texture2D (tex, v_texcoord);
    vec2 px = vec2 (v_texcoord.x * width, v_texcoord.y * height);
    float grain = hash (floor (px)) * 0.5 + hash (floor (px * 0.5)) * 0.5;
    float fibre = hash (vec2 (floor (px.y), 0.0)) * 0.15;
    float paper = 0.92 + grain * (grain / 100.0) * 0.12 - fibre;
    vec3 tinted = c.rgb * mix (1.0, paper, tint / 100.0);
    gl_FragColor = vec4 (mix (c.rgb, tinted, strength / 100.0), c.a);
}
