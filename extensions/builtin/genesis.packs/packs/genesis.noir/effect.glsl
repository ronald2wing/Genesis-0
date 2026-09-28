// SPDX-License-Identifier: GPL-3.0-or-later
// SPDX-FileCopyrightText: 2026 Genesis-0 contributors
//
// The fragment BODY; see genesis.exposure/effect.glsl for the contract.

void main () {
    vec4 c = texture2D (tex, v_texcoord);
    float l = dot (c.rgb, vec3 (0.2126, 0.7152, 0.0722));
    float s = strength / 100.0;
    float g = clamp ((l - 0.5) * (1.0 + 1.8 * s) + 0.5, 0.0, 1.0);
    vec3 noir = vec3 (g) * vec3 (0.95, 0.99, 1.05);
    gl_FragColor = vec4 (mix (c.rgb, noir, s), c.a);
}
