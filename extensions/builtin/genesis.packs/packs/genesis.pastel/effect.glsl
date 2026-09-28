// SPDX-License-Identifier: GPL-3.0-or-later
// SPDX-FileCopyrightText: 2026 Genesis-0 contributors
//
// The fragment BODY; see genesis.exposure/effect.glsl for the contract.

void main () {
    vec4 c = texture2D (tex, v_texcoord);
    float l = dot (c.rgb, vec3 (0.2126, 0.7152, 0.0722));
    vec3 soft = mix (c.rgb, vec3 (l), 0.35);
    vec3 lifted = soft + vec3 (0.08, 0.08, 0.1);
    gl_FragColor = vec4 (mix (c.rgb, lifted, strength / 100.0), c.a);
}
