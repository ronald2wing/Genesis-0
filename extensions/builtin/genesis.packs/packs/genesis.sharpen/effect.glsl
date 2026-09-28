// SPDX-License-Identifier: GPL-3.0-or-later
// SPDX-FileCopyrightText: 2026 Genesis-0 contributors
//
// The fragment BODY; see genesis.exposure/effect.glsl for the contract.
// `width` and `height` are set by glshader every frame, not by the pack.

uniform float width;
uniform float height;

void main () {
    vec2 t = vec2 (1.0 / width, 1.0 / height);
    vec3 c = texture2D (tex, v_texcoord).rgb;
    vec3 blur = vec3 (0.0);
    float total = 0.0;
    for (int y = -2; y <= 2; y++) {
        for (int x = -2; x <= 2; x++) {
            float w = exp (-float (x * x + y * y) / 2.0);
            blur += texture2D (tex, v_texcoord + vec2 (float (x), float (y)) * t).rgb * w;
            total += w;
        }
    }
    gl_FragColor = vec4 (c + (c - blur / total) * amount, 1.0);
}
