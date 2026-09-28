// SPDX-License-Identifier: GPL-3.0-or-later
// SPDX-FileCopyrightText: 2026 Genesis-0 contributors
//
// The fragment BODY; see genesis.exposure/effect.glsl for the contract.

void main () {
    vec4 c = texture2D (tex, v_texcoord);
    float l = dot (c.rgb, vec3 (0.2126, 0.7152, 0.0722));
    float inv = step (threshold / 100.0, l);
    vec3 solar = mix (c.rgb, 1.0 - c.rgb, inv);
    gl_FragColor = vec4 (mix (c.rgb, solar, strength / 100.0), c.a);
}
