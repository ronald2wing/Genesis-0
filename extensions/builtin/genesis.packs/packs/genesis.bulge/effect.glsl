// SPDX-License-Identifier: GPL-3.0-or-later
// SPDX-FileCopyrightText: 2026 Genesis-0 contributors
//
// The fragment BODY; see genesis.exposure/effect.glsl for the contract.

void main () {
    vec2 p = v_texcoord - vec2 (center_x, center_y);
    float r = length (p);
    float s = strength / 100.0;
    float k = pow (r, 1.0 / (1.0 + s));
    vec2 uv = vec2 (center_x, center_y) + p * (k / max (r, 0.0001));
    gl_FragColor = texture2D (tex, uv);
}
