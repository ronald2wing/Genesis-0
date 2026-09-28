// SPDX-License-Identifier: GPL-3.0-or-later
// SPDX-FileCopyrightText: 2026 Genesis-0 contributors
//
// Re-authored from Concat's concat-effects package `concat.neon`,
// SPDX-License-Identifier: GPL-3.0-or-later,
// SPDX-FileCopyrightText: 2026 Jareer and Concat contributors.
//
// The fragment BODY; see genesis.exposure/effect.glsl for the contract.
// The glow folded into the one pass: a small bright-halation window over the
// same texture, where the WGSL original ran a separate halation stage.
// `width` and `height` are set by glshader every frame, not by the pack.

uniform float width;
uniform float height;

void main () {
    vec2 t = vec2 (1.0 / width, 1.0 / height);
    vec3 c = texture2D (tex, v_texcoord).rgb;
    float g = glow / 100.0;
    float punch = punch / 100.0;
    float l = dot (c, vec3 (0.2126, 0.7152, 0.0722));
    vec3 split = c
        + vec3 (0.18, -0.1, 0.18) * (1.0 - l) * g * 0.5
        + vec3 (-0.1, 0.1, 0.15) * l * g * 0.5;
    split = (split - vec3 (0.5)) * (1.0 + punch * 0.5) + vec3 (0.5);
    float gray = dot (split, vec3 (0.2126, 0.7152, 0.0722));
    split = mix (vec3 (gray), split, 1.0 + punch * 0.6);
    vec3 sum = vec3 (0.0);
    for (int y = -1; y <= 1; y++) {
        for (int x = -1; x <= 1; x++) {
            vec3 q = texture2D (tex, v_texcoord + vec2 (float (x), float (y)) * t).rgb;
            float bright = smoothstep (0.55, 1.0, dot (q, vec3 (0.2126, 0.7152, 0.0722)));
            sum += q * bright;
        }
    }
    vec3 bloom = sum / 9.0 * g * 0.4;
    vec3 out = 1.0 - (1.0 - split) * (1.0 - bloom);
    gl_FragColor = vec4 (clamp (out, 0.0, 1.0), 1.0);
}
