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
    vec2 cell = floor (vec2 (v_texcoord.x * width / grain, v_texcoord.y * height / grain));
    float n = hash (cell) - 0.5;
    vec3 result = c.rgb + n * (strength / 100.0) * 0.6;
    gl_FragColor = vec4 (result, c.a);
}
