// SPDX-License-Identifier: GPL-3.0-or-later
// SPDX-FileCopyrightText: 2026 Genesis-0 contributors
//
// The fragment BODY; see genesis.exposure/effect.glsl for the contract.

void main () {
    vec2 p = v_texcoord - vec2 (0.5);
    float r = length (p);
    float wave = sin (r * frequency * 6.28318 + radians (phase)) * amplitude / 100.0 * 0.05;
    vec2 uv = v_texcoord + normalize (p + vec2 (0.0001)) * wave;
    gl_FragColor = texture2D (tex, uv);
}
