// SPDX-License-Identifier: GPL-3.0-or-later
// SPDX-FileCopyrightText: 2026 Genesis-0 contributors
//
// The fragment BODY; see genesis.exposure/effect.glsl for the contract.

void main () {
    vec4 c = texture2D (tex, v_texcoord);
    vec2 centre = vec2 (center_x / 100.0, center_y / 100.0);
    vec2 d = v_texcoord - centre;
    float spread = (amount / 100.0) * 0.25;
    vec3 acc = c.rgb;
    float total = 1.0;
    for (int i = 1; i < 8; i++) {
        float f = float (i) / 7.0;
        float scale = 1.0 - spread * f;
        vec2 sample = centre + d * scale;
        float w = 1.0 - f;
        acc += texture2D (tex, clamp (sample, 0.0, 1.0)).rgb * w;
        total += w;
    }
    vec3 blurred = acc / total;
    gl_FragColor = vec4 (mix (c.rgb, blurred, strength / 100.0), c.a);
}
