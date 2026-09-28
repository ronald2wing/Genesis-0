// SPDX-License-Identifier: GPL-3.0-or-later
// SPDX-FileCopyrightText: 2026 Genesis-0 contributors
//
// The fragment BODY; see genesis.exposure/effect.glsl for the contract.

void main () {
    vec4 c = texture2D (tex, v_texcoord);
    vec3 mixed = vec3 (
        rr * c.r + rg * c.g + rb * c.b,
        gr * c.r + gg * c.g + gb * c.b,
        br * c.r + bg * c.g + bb * c.b);
    gl_FragColor = vec4 (clamp (mixed, 0.0, 1.0), c.a);
}
