// SPDX-License-Identifier: GPL-3.0-or-later
// SPDX-FileCopyrightText: 2026 Genesis-0 contributors
//
// The fragment BODY; see genesis.exposure/effect.glsl for the contract.

vec3 hue_rgb (float h) {
    return clamp (abs (mod (h * 6.0 + vec3 (0.0, 4.0, 2.0), 6.0) - 3.0) - 1.0, 0.0, 1.0);
}

void main () {
    vec4 c = texture2D (tex, v_texcoord);
    vec3 top = hue_rgb (top_hue / 360.0);
    vec3 bot = hue_rgb (bottom_hue / 360.0);
    vec3 tint = mix (top, bot, v_texcoord.y);
    float s = strength / 100.0;
    vec3 graded = c.rgb * mix (vec3 (1.0), tint * 1.6, s * 0.6);
    gl_FragColor = vec4 (mix (c.rgb, graded, s), c.a);
}
