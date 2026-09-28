// SPDX-License-Identifier: GPL-3.0-or-later
// SPDX-FileCopyrightText: 2026 Genesis-0 contributors
//
// The fragment BODY; see genesis.exposure/effect.glsl for the contract.

void main () {
    vec2 uv = v_texcoord - vec2 (0.5);
    float r = length (uv);
    float a = atan (uv.y, uv.x);
    a += strength / 100.0 * 6.28318 * (1.0 - r);
    vec2 k = vec2 (cos (a), sin (a)) * r + vec2 (0.5);
    gl_FragColor = texture2D (tex, k);
}
