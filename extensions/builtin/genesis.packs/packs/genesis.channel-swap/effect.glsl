// SPDX-License-Identifier: GPL-3.0-or-later
// SPDX-FileCopyrightText: 2026 Genesis-0 contributors
//
// The fragment BODY; see genesis.exposure/effect.glsl for the contract.

void main () {
    vec4 c = texture2D (tex, v_texcoord);
    vec3 rgb;
    if (mode < 0.5) {
        rgb = vec3 (c.r, c.b, c.g);
    } else if (mode < 1.5) {
        rgb = vec3 (c.g, c.b, c.r);
    } else if (mode < 2.5) {
        rgb = vec3 (c.b, c.r, c.g);
    } else if (mode < 3.5) {
        rgb = vec3 (c.b, c.g, c.r);
    } else if (mode < 4.5) {
        rgb = vec3 (c.g, c.r, c.b);
    } else {
        rgb = c.rgb;
    }
    gl_FragColor = vec4 (rgb, c.a);
}
