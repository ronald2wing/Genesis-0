// SPDX-License-Identifier: GPL-3.0-or-later
// SPDX-FileCopyrightText: 2026 Genesis-0 contributors
//
// The fragment BODY; see genesis.exposure/effect.glsl for the contract.
// `width` and `height` are set by glshader every frame, not by the pack.

uniform float width;
uniform float height;

void main () {
    vec2 t = vec2 (1.0 / width, 1.0 / height);
    vec4 c = texture2D (tex, v_texcoord);
    vec3 sum = vec3 (0.0);
    float total = 0.0;
    for (int y = -3; y <= 3; y++) {
        for (int x = -3; x <= 3; x++) {
            vec3 s = texture2D (tex, v_texcoord + vec2 (float (x), float (y)) * t * radius).rgb;
            float w = 1.0 / (1.0 + length (s - c.rgb) * sharpness);
            sum += s * w;
            total += w;
        }
    }
    gl_FragColor = vec4 (sum / total, c.a);
}
