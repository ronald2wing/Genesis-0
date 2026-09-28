// SPDX-License-Identifier: GPL-3.0-or-later
// SPDX-FileCopyrightText: 2026 Genesis-0 contributors
//
// The fragment BODY; see genesis.exposure/effect.glsl for the contract.

void main () {
    vec4 c = texture2D (tex, v_texcoord);
    vec2 d = vec2 (min (v_texcoord.x, 1.0 - v_texcoord.x), min (v_texcoord.y, 1.0 - v_texcoord.y));
    float m = min (d.x, d.y);
    float start = 1.0 - (offset / 100.0);
    float soft = (softness / 100.0) * 0.3;
    float shadow = 1.0 - smoothstep (start, start + soft, m);
    vec3 result = c.rgb * (1.0 - shadow * 0.7);
    gl_FragColor = vec4 (mix (c.rgb, result, strength / 100.0), c.a);
}
