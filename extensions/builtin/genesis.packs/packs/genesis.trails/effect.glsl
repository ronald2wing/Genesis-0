// SPDX-License-Identifier: GPL-3.0-or-later
// SPDX-FileCopyrightText: 2026 Genesis-0 contributors
//
// The fragment BODY; see genesis.exposure/effect.glsl for the contract.

void main () {
    vec4 c = texture2D (tex, v_texcoord);
    float rad = angle * 3.14159 / 180.0;
    vec2 dir = vec2 (cos (rad), sin (rad)) * (length / 100.0) * 0.12;
    vec3 acc = c.rgb;
    float total = 1.0;
    for (int i = 1; i < 10; i++) {
        vec2 off = dir * (float (i) / 9.0);
        acc += texture2D (tex, clamp (v_texcoord - off, 0.0, 1.0)).rgb;
        acc += texture2D (tex, clamp (v_texcoord + off, 0.0, 1.0)).rgb;
        total += 2.0;
    }
    vec3 blurred = acc / total;
    gl_FragColor = vec4 (mix (c.rgb, blurred, strength / 100.0), c.a);
}
