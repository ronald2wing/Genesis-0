// SPDX-License-Identifier: GPL-3.0-or-later
// SPDX-FileCopyrightText: 2026 Genesis-0 contributors
//
// The fragment BODY; see genesis.exposure/effect.glsl for the contract.

void main () {
    vec2 p = v_texcoord - vec2 (center_x, center_y);
    float r = length (p);
    float a = atan (p.y, p.x);
    a += strength / 100.0 * 6.28318 * r;
    vec2 uv = vec2 (cos (a), sin (a)) * r + vec2 (center_x, center_y);
    gl_FragColor = texture2D (tex, uv);
}
