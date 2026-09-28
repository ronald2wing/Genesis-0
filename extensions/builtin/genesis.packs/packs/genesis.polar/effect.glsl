// SPDX-License-Identifier: GPL-3.0-or-later
// SPDX-FileCopyrightText: 2026 Genesis-0 contributors
//
// The fragment BODY; see genesis.exposure/effect.glsl for the contract.

void main () {
    vec4 c = texture2D (tex, v_texcoord);
    vec2 centred = v_texcoord - 0.5;
    float r = length (centred) * 2.0;
    float a = atan (centred.y, centred.x);
    vec2 polar = vec2 ((a + 3.14159) / (2.0 * 3.14159), r);
    vec3 warped = texture2D (tex, clamp (polar, 0.0, 1.0)).rgb;
    gl_FragColor = vec4 (mix (c.rgb, warped, strength / 100.0), c.a);
}
