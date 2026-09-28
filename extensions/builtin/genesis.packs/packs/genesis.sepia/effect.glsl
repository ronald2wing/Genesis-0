// SPDX-License-Identifier: GPL-3.0-or-later
// SPDX-FileCopyrightText: 2026 Genesis-0 contributors
//
// The fragment BODY; see genesis.exposure/effect.glsl for the contract.

void main () {
    vec4 c = texture2D (tex, v_texcoord);
    vec3 s = vec3 (
        dot (c.rgb, vec3 (0.393, 0.769, 0.189)),
        dot (c.rgb, vec3 (0.349, 0.686, 0.168)),
        dot (c.rgb, vec3 (0.272, 0.534, 0.131)));
    gl_FragColor = vec4 (mix (c.rgb, s, strength / 100.0), c.a);
}
