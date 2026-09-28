// SPDX-License-Identifier: GPL-3.0-or-later
// SPDX-FileCopyrightText: 2026 Genesis-0 contributors
//
// The fragment BODY; see genesis.exposure/effect.glsl for the contract.

void main () {
    vec2 uv = v_texcoord - vec2 (center_x, center_y);
    float r = max (radius / 100.0, 0.05);
    vec2 p = uv / vec2 (r, r * 0.75);
    float d = length (p);
    float s = strength / 100.0;
    float fall = smoothstep (0.3, 1.2, d);
    vec3 c = texture2D (tex, v_texcoord).rgb;
    gl_FragColor = vec4 (c * (1.0 - fall * 0.95 * s), 1.0);
}
