// SPDX-License-Identifier: GPL-3.0-or-later
// SPDX-FileCopyrightText: 2026 Genesis-0 contributors
//
// The fragment BODY; see genesis.exposure/effect.glsl for the contract.
// Each knob bends its own tonal range: shadows near black, midtones at
// middle grey, highlights near white, each weighted so the ranges stay
// monotone and do not fight.

void main () {
    vec4 c = texture2D (tex, v_texcoord);
    vec3 rgb = c.rgb;
    float w_shadow = 1.0 - smoothstep (0.0, 0.5, rgb);
    float w_mid = 1.0 - abs (rgb * 2.0 - 1.0);
    float w_highlight = smoothstep (0.5, 1.0, rgb);
    rgb = rgb
        + shadows * w_shadow * 0.5
        + midtones * w_mid * 0.5
        + highlights * w_highlight * 0.5;
    gl_FragColor = vec4 (clamp (rgb, 0.0, 1.0), c.a);
}
