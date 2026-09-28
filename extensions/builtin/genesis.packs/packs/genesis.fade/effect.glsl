// SPDX-License-Identifier: GPL-3.0-or-later
// SPDX-FileCopyrightText: 2026 Genesis-0 contributors
//
// The fragment BODY; see genesis.exposure/effect.glsl for the contract.
// The blacks are lifted, the whites pulled down and a touch of saturation
// removed, the whole grade mixed back by strength.

void main () {
    vec4 c = texture2D (tex, v_texcoord);
    float s = strength / 100.0;
    vec3 faded = c.rgb * (1.0 - 0.18 * s) + vec3 (0.10 * s);
    float l = dot (faded, vec3 (0.2126, 0.7152, 0.0722));
    faded = mix (faded, vec3 (l), 0.25 * s);
    gl_FragColor = vec4 (mix (c.rgb, faded, s), c.a);
}
