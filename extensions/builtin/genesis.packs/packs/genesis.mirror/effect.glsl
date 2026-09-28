// SPDX-License-Identifier: GPL-3.0-or-later
// SPDX-FileCopyrightText: 2026 Genesis-0 contributors
//
// The fragment BODY; see genesis.exposure/effect.glsl for the contract.
// `mode` is an enum parameter, laid out as a float uniform by the resolver.

void main () {
    vec2 uv = v_texcoord;
    float m = floor (mode + 0.5);
    if (m == 0.0) {
        uv.x = abs (uv.x * 2.0 - 1.0);
    } else if (m == 1.0) {
        uv.y = abs (uv.y * 2.0 - 1.0);
    } else {
        uv.x = abs (uv.x * 2.0 - 1.0);
        uv.y = abs (uv.y * 2.0 - 1.0);
    }
    gl_FragColor = texture2D (tex, uv);
}
