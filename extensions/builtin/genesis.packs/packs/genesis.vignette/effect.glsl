// SPDX-License-Identifier: GPL-3.0-or-later
// SPDX-FileCopyrightText: 2026 Genesis-0 contributors
//
// The fragment BODY; see genesis.exposure/effect.glsl for the contract.

void main () {
    vec4 c = texture2D (tex, v_texcoord);
    float d = length ((v_texcoord - vec2 (0.5)) * vec2 (2.0));
    float s = strength_norm;
    float fall = smoothstep (1.4 - s * 0.9, 1.4 + 0.2 - s * 0.3, d);
    gl_FragColor = vec4 (c.rgb * (1.0 - fall * (0.4 + 0.6 * s)), c.a);
}
