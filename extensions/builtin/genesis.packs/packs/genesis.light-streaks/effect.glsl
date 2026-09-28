// SPDX-License-Identifier: GPL-3.0-or-later
// SPDX-FileCopyrightText: 2026 Genesis-0 contributors
//
// The fragment BODY; see genesis.exposure/effect.glsl for the contract.

uniform float width;
uniform float height;

void main () {
    vec4 c = texture2D (tex, v_texcoord);
    float thresh = threshold / 100.0;
    float spread = (length / 100.0) * 0.5;
    vec2 px = vec2 (1.0 / width, 1.0 / height);
    vec3 streaks = vec3 (0.0);
    for (int i = -8; i <= 8; i++) {
        float t = float (i) / 8.0;
        vec2 sample = v_texcoord + vec2 (t * spread, 0.0);
        vec3 s = texture2D (tex, clamp (sample, 0.0, 1.0)).rgb;
        float l = dot (s, vec3 (0.2126, 0.7152, 0.0722));
        float w = 1.0 - abs (t);
        streaks += s * smoothstep (thresh, 1.0, l) * w;
    }
    streaks /= 9.0;
    vec3 result = c.rgb + streaks * (strength / 100.0) * 1.2;
    gl_FragColor = vec4 (result, c.a);
}
