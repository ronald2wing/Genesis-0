// SPDX-License-Identifier: GPL-3.0-or-later
// SPDX-FileCopyrightText: 2026 Genesis-0 contributors
//
// The fragment BODY; see genesis.exposure/effect.glsl for the contract.

void main () {
    vec4 c = texture2D (tex, v_texcoord);
    vec3 rgb = c.rgb;
    float mx = max (max (rgb.r, rgb.g), rgb.b);
    float mn = min (min (rgb.r, rgb.g), rgb.b);
    float l = dot (rgb, vec3 (0.2126, 0.7152, 0.0722));
    float k = 1.0 + amount * max (1.0 - (mx - mn), 0.0);
    gl_FragColor = vec4 (mix (vec3 (l), rgb, k), c.a);
}
