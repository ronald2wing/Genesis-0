// SPDX-License-Identifier: GPL-3.0-or-later
// SPDX-FileCopyrightText: 2026 Genesis-0 contributors
//
// The fragment BODY; see genesis.exposure/effect.glsl for the contract.

void main () {
    float s = 1.0 + amount / 100.0;
    vec2 uv = v_texcoord - vec2 (0.5);
    if (axis < 0.5) {
        uv.x *= s;
    } else {
        uv.y *= s;
    }
    uv += vec2 (0.5);
    gl_FragColor = texture2D (tex, uv);
}
