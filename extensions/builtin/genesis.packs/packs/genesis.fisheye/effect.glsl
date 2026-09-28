// SPDX-License-Identifier: GPL-3.0-or-later
// SPDX-FileCopyrightText: 2026 Genesis-0 contributors
//
// The fragment BODY; see genesis.exposure/effect.glsl for the contract.

void main () {
    vec2 uv = v_texcoord;
    float s = strength / 100.0 * 0.6;
    vec2 mid = uv - vec2 (0.5);
    float r = length (mid) / 0.7071;
    float pulled = (1.0 - s) + s * r * r;
    gl_FragColor = texture2D (tex, vec2 (0.5) + mid * pulled);
}
