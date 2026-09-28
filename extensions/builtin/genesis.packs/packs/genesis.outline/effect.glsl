// SPDX-License-Identifier: GPL-3.0-or-later
// SPDX-FileCopyrightText: 2026 Genesis-0 contributors
//
// The fragment BODY; see genesis.exposure/effect.glsl for the contract.
// `width` and `height` are set by glshader every frame, not by the pack.

uniform float width;
uniform float height;

void main () {
    vec2 t = vec2 (1.0 / width, 1.0 / height) * thickness;
    vec4 c = texture2D (tex, v_texcoord);
    float maxa = 0.0;
    for (int y = -2; y <= 2; y++) {
        for (int x = -2; x <= 2; x++) {
            maxa = max (maxa, texture2D (tex, v_texcoord + vec2 (float (x), float (y)) * t).a);
        }
    }
    float edge = maxa - c.a;
    vec3 col = vec3 (1.0);
    if (color < 0.5) {
        col = vec3 (1.0);
    } else if (color < 1.5) {
        col = vec3 (0.0);
    } else if (color < 2.5) {
        col = vec3 (1.0, 0.0, 0.0);
    } else if (color < 3.5) {
        col = vec3 (1.0, 1.0, 0.0);
    } else if (color < 4.5) {
        col = vec3 (0.0, 1.0, 1.0);
    } else {
        col = vec3 (1.0, 0.0, 1.0);
    }
    float k = edge * (strength / 100.0);
    gl_FragColor = vec4 (mix (c.rgb, col, k), max (c.a, k));
}
