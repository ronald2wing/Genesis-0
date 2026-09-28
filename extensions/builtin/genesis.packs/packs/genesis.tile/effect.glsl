// SPDX-License-Identifier: GPL-3.0-or-later
// SPDX-FileCopyrightText: 2026 Genesis-0 contributors
//
// The fragment BODY; see genesis.exposure/effect.glsl for the contract.

void main () {
    vec4 c = texture2D (tex, v_texcoord);
    vec2 tiled = fract (v_texcoord * vec2 (columns, rows));
    vec3 repeated = texture2D (tex, tiled).rgb;
    gl_FragColor = vec4 (mix (c.rgb, repeated, strength / 100.0), c.a);
}
