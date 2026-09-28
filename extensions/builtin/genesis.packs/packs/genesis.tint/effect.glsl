// SPDX-License-Identifier: GPL-3.0-or-later
// SPDX-FileCopyrightText: 2026 Genesis-0 contributors
//
// The fragment BODY; see genesis.exposure/effect.glsl for the contract.
// Positive is towards magenta, negative towards green, by lifting the red
// and blue channels relative to green.

void main () {
    vec4 c = texture2D (tex, v_texcoord);
    float t = amount / 100.0;
    vec3 g = vec3 (1.0 + 0.2 * t, 1.0 - 0.2 * t, 1.0 + 0.2 * t);
    gl_FragColor = vec4 (c.rgb * g, c.a);
}
