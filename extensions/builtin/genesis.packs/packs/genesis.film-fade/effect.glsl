// SPDX-License-Identifier: GPL-3.0-or-later
// SPDX-FileCopyrightText: 2026 Genesis-0 contributors
//
// The fragment BODY; see genesis.exposure/effect.glsl for the contract.

void main () {
    vec4 c = texture2D (tex, v_texcoord);
    float s = strength / 100.0;
    float l = dot (c.rgb, vec3 (0.2126, 0.7152, 0.0722));
    vec3 faded = c.rgb * (1.0 - 0.25 * s) + vec3 (0.05, 0.04, 0.03) * s;
    faded = mix (faded, vec3 (l), 0.35 * s);
    faded = mix (faded, vec3 (1.0, 0.9, 0.74), 0.1 * s);
    gl_FragColor = vec4 (mix (c.rgb, faded, s), c.a);
}
