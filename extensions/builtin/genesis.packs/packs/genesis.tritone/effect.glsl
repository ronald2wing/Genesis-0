// SPDX-License-Identifier: GPL-3.0-or-later
// SPDX-FileCopyrightText: 2026 Genesis-0 contributors
//
// The fragment BODY; see genesis.exposure/effect.glsl for the contract.

vec3 hue_rgb (float h) {
    return clamp (abs (mod (h * 6.0 + vec3 (0.0, 4.0, 2.0), 6.0) - 3.0) - 1.0, 0.0, 1.0);
}

void main () {
    vec4 c = texture2D (tex, v_texcoord);
    float l = dot (c.rgb, vec3 (0.2126, 0.7152, 0.0722));
    vec3 lo = hue_rgb (shadow_hue / 360.0);
    vec3 mid = hue_rgb (mid_hue / 360.0);
    vec3 hi = hue_rgb (highlight_hue / 360.0);
    vec3 mapped = l < 0.5 ? mix (lo, mid, l * 2.0) : mix (mid, hi, l * 2.0 - 1.0);
    gl_FragColor = vec4 (mapped, c.a);
}
