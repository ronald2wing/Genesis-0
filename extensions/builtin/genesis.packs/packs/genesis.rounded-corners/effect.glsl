// SPDX-License-Identifier: GPL-3.0-or-later
// SPDX-FileCopyrightText: 2026 Genesis-0 contributors
//
// The fragment BODY; see genesis.exposure/effect.glsl for the contract.

void main () {
    vec4 c = texture2D (tex, v_texcoord);
    float r = radius / 100.0;
    vec2 corner = vec2 (min (v_texcoord.x, 1.0 - v_texcoord.x), min (v_texcoord.y, 1.0 - v_texcoord.y));
    float d = length (max (corner - vec2 (1.0 - r), 0.0));
    float inside = 1.0 - smoothstep (r - 0.02, r, d);
    vec3 result = c.rgb * inside;
    gl_FragColor = vec4 (mix (c.rgb, result, strength / 100.0), c.a);
}
