// SPDX-License-Identifier: GPL-3.0-or-later
// SPDX-FileCopyrightText: 2026 Genesis-0 contributors
//
// The fragment BODY; see genesis.exposure/effect.glsl for the contract.

void main () {
    vec4 c = texture2D (tex, v_texcoord);
    float t = amount / 100.0;
    vec2 p = v_texcoord;
    p.x += (p.y - 0.5) * t;
    vec3 warped = texture2D (tex, clamp (p, 0.0, 1.0)).rgb;
    gl_FragColor = vec4 (mix (c.rgb, warped, strength / 100.0), c.a);
}
