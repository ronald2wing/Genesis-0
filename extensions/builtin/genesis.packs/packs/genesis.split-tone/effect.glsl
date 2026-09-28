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
    float b = balance / 100.0;
    float shadow = (1.0 - smoothstep (0.0, 0.5, l)) * b;
    float highlight = smoothstep (0.5, 1.0, l) * b;
    vec3 tinted = c.rgb;
    tinted = mix (tinted, hue_rgb (shadow_hue / 360.0), shadow);
    tinted = mix (tinted, hue_rgb (highlight_hue / 360.0), highlight);
    gl_FragColor = vec4 (tinted, c.a);
}
