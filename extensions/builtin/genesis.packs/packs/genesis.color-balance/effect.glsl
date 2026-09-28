// SPDX-License-Identifier: GPL-3.0-or-later
// SPDX-FileCopyrightText: 2026 Genesis-0 contributors
//
// Re-authored from Concat's concat-effects package `concat.color-wheels`,
// SPDX-License-Identifier: GPL-3.0-or-later,
// SPDX-FileCopyrightText: 2026 Jareer and Concat contributors.
//
// The fragment BODY; see genesis.exposure/effect.glsl for the contract.
// The wheels' lift/gamma/gain grade, as three scalar knobs not pucks.

void main () {
    vec4 c = texture2D (tex, v_texcoord);
    vec3 rgb = c.rgb;
    rgb = rgb + lift * (1.0 - rgb);
    rgb = pow (clamp (rgb, 0.0, 1.0), vec3 (1.0 / gamma));
    rgb = rgb * gain;
    gl_FragColor = vec4 (rgb, c.a);
}
