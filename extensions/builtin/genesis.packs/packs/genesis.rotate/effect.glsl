// SPDX-License-Identifier: GPL-3.0-or-later
// SPDX-FileCopyrightText: 2026 Genesis-0 contributors
//
// The fragment BODY; see genesis.exposure/effect.glsl for the contract.
// `mode` is an enum parameter, laid out as a float uniform by the resolver.
// The turn is about the middle of the frame.

void main () {
    vec2 uv = v_texcoord - 0.5;
    float m = floor (mode + 0.5);
    if (m == 0.0) {
        uv = vec2 (-uv.y, uv.x);
    } else if (m == 1.0) {
        uv = -uv;
    } else {
        uv = vec2 (uv.y, -uv.x);
    }
    gl_FragColor = texture2D (tex, uv + 0.5);
}
