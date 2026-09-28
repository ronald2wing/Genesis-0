// SPDX-License-Identifier: GPL-3.0-or-later
// SPDX-FileCopyrightText: 2026 Genesis-0 contributors
//
// The fragment BODY; see genesis.exposure/effect.glsl for the contract.

uniform float width;
uniform float height;

void main () {
    vec4 c = texture2D (tex, v_texcoord);
    float thresh = threshold / 100.0;
    vec2 px = vec2 (radius / width, radius / height);
    vec3 glow = vec3 (0.0);
    for (int i = -4; i <= 4; i++) {
        for (int j = -4; j <= 4; j++) {
            vec3 s = texture2D (tex, clamp (v_texcoord + vec2 (float (i), float (j)) * px, 0.0, 1.0)).rgb;
            float l = dot (s, vec3 (0.2126, 0.7152, 0.0722));
            glow += s * smoothstep (thresh, 1.0, l);
        }
    }
    glow /= 81.0;
    vec3 result = c.rgb + glow * (strength / 100.0) * 1.5;
    gl_FragColor = vec4 (result, c.a);
}
