// SPDX-License-Identifier: GPL-3.0-or-later
// SPDX-FileCopyrightText: 2026 Genesis-0 contributors
//
// The fragment BODY; see genesis.exposure/effect.glsl for the contract.
// Brightness adds, contrast spreads about middle grey, saturation mixes
// towards or away from each pixel's own luminance.

void main () {
    vec4 c = texture2D (tex, v_texcoord);
    vec3 rgb = c.rgb + brightness / 100.0;
    rgb = (rgb - 0.5) * (1.0 + contrast / 100.0) + 0.5;
    float l = dot (rgb, vec3 (0.2126, 0.7152, 0.0722));
    rgb = mix (vec3 (l), rgb, 1.0 + saturation / 100.0);
    gl_FragColor = vec4 (clamp (rgb, 0.0, 1.0), c.a);
}
