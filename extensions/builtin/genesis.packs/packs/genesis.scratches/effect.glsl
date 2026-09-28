// SPDX-License-Identifier: GPL-3.0-or-later
// SPDX-FileCopyrightText: 2026 Genesis-0 contributors
//
// The fragment BODY; see genesis.exposure/effect.glsl for the contract.

uniform float width;
uniform float height;

float hash (vec2 p) {
    return fract (sin (dot (p, vec2 (127.1, 311.7))) * 43758.5453123);
}

void main () {
    vec4 c = texture2D (tex, v_texcoord);
    float x = v_texcoord.x * width;
    float line = hash (vec2 (floor (x / 8.0), 13.0));
    float is_line = line > 1.0 - (amount / 100.0) * 0.4 ? 1.0 : 0.0;
    float wiggle = (hash (vec2 (floor (x / 8.0), floor (v_texcoord.y * height / 40.0))) - 0.5) * 6.0;
    float pos = x / 8.0 + wiggle / 8.0;
    float band = 1.0 - smoothstep (0.2, 1.4, abs (fract (pos) - 0.5) * 2.0);
    float scratch = is_line * band;
    vec3 result = mix (c.rgb, vec3 (1.0), scratch * 0.6);
    gl_FragColor = vec4 (mix (c.rgb, result, strength / 100.0), c.a);
}
