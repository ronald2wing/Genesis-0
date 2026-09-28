// SPDX-License-Identifier: GPL-3.0-or-later
// SPDX-FileCopyrightText: 2026 Genesis-0 contributors
//
// The fragment BODY; see genesis.exposure/effect.glsl for the contract.
// `amplitude` scales the ripple as a share of the frame and `frequency` is
// the number of full waves across it. `direction` is an enum parameter,
// laid out as a float uniform by the resolver.

void main () {
    vec2 uv = v_texcoord;
    float m = floor (direction + 0.5);
    float amp = amplitude / 100.0 * 0.1;
    float wave = 6.2831853 * frequency;
    if (m == 0.0) {
        uv.y += amp * sin (uv.x * wave);
    } else {
        uv.x += amp * sin (uv.y * wave);
    }
    gl_FragColor = texture2D (tex, uv);
}
