// SPDX-License-Identifier: GPL-3.0-or-later
// SPDX-FileCopyrightText: 2026 Genesis-0 contributors
//
// The fragment BODY; see genesis.exposure/effect.glsl for the contract.
// The highlights' light gathered over a 5x5 window and screened back, the
// multi-pass Gaussian folded into one. `width` and `height` are set by
// glshader every frame, not by the pack.

uniform float width;
uniform float height;

void main () {
    vec2 t = vec2 (1.0 / width, 1.0 / height);
    vec3 c = texture2D (tex, v_texcoord).rgb;
    vec3 sum = vec3 (0.0);
    for (int y = -2; y <= 2; y++) {
        for (int x = -2; x <= 2; x++) {
            vec3 s = texture2D (tex, v_texcoord + vec2 (float (x), float (y)) * t).rgb;
            float bright = smoothstep (0.55, 1.0, dot (s, vec3 (0.2126, 0.7152, 0.0722)));
            sum += s * bright;
        }
    }
    vec3 glow = clamp (sum / 25.0 * (amount / 100.0), 0.0, 1.0);
    gl_FragColor = vec4 (1.0 - (1.0 - c) * (1.0 - glow), 1.0);
}
