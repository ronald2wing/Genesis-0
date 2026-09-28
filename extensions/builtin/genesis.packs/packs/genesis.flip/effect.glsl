// SPDX-License-Identifier: GPL-3.0-or-later
// SPDX-FileCopyrightText: 2026 Genesis-0 contributors
//
// The fragment BODY; see genesis.exposure/effect.glsl for the contract.
// `mode` is an enum parameter, laid out as a float uniform by the resolver.
// A flip reverses the picture; it does not reflect it the way a mirror does.

void main () {
    vec2 uv = v_texcoord;
    float m = floor (mode + 0.5);
    if (m == 0.0) {
        uv.x = 1.0 - uv.x;
    } else if (m == 1.0) {
        uv.y = 1.0 - uv.y;
    } else {
        uv = vec2 (1.0 - uv.x, 1.0 - uv.y);
    }
    gl_FragColor = texture2D (tex, uv);
}
