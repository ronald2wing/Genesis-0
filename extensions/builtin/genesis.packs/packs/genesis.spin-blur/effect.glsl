// SPDX-License-Identifier: GPL-3.0-or-later
// SPDX-FileCopyrightText: 2026 Genesis-0 contributors
//
// The fragment BODY; see genesis.exposure/effect.glsl for the contract.

void main () {
    vec4 c = texture2D (tex, v_texcoord);
    vec2 centre = vec2 (center_x / 100.0, center_y / 100.0);
    vec2 d = v_texcoord - centre;
    float r = length (d);
    float base = atan (d.y, d.x);
    float spread = (amount / 100.0) * 0.6 * r;
    vec3 acc = vec3 (0.0);
    for (int i = -4; i <= 4; i++) {
        float a = base + spread * (float (i) / 4.0);
        vec2 sample = centre + vec2 (cos (a), sin (a)) * r;
        acc += texture2D (tex, clamp (sample, 0.0, 1.0)).rgb;
    }
    vec3 blurred = acc / 9.0;
    gl_FragColor = vec4 (mix (c.rgb, blurred, strength / 100.0), c.a);
}
