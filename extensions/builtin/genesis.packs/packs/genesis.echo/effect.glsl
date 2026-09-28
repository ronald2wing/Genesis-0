// SPDX-License-Identifier: GPL-3.0-or-later
// SPDX-FileCopyrightText: 2026 Genesis-0 contributors
//
// The fragment BODY; see genesis.exposure/effect.glsl for the contract.

void main () {
    vec4 c = texture2D (tex, v_texcoord);
    float rad = angle * 3.14159 / 180.0;
    vec2 dir = vec2 (cos (rad), sin (rad));
    vec2 offset = dir * (distance / 100.0) * 0.2;
    vec3 acc = vec3 (0.0);
    float weight = 0.0;
    for (int i = 1; i < 8; i++) {
        float f = float (i);
        float w = 1.0 / f;
        acc += texture2D (tex, clamp (v_texcoord - offset * f, 0.0, 1.0)).rgb * w;
        weight += w;
    }
    vec3 echo = acc / weight;
    gl_FragColor = vec4 (mix (c.rgb, echo, strength / 100.0), c.a);
}
