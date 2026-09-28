// SPDX-License-Identifier: GPL-3.0-or-later
// SPDX-FileCopyrightText: 2026 Genesis-0 contributors
//
// The fragment BODY; see genesis.exposure/effect.glsl for the contract.

void main () {
    vec4 c = texture2D (tex, v_texcoord);
    float tear = tear_y / 100.0;
    float below = v_texcoord.y < tear ? 1.0 : 0.0;
    vec2 shifted = vec2 (v_texcoord.x + below * (offset / 100.0) * 0.3, v_texcoord.y);
    vec3 torn = texture2D (tex, clamp (shifted, 0.0, 1.0)).rgb;
    gl_FragColor = vec4 (mix (c.rgb, torn, strength / 100.0), c.a);
}
