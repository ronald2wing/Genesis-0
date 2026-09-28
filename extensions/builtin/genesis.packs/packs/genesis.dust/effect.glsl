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
    vec2 px = vec2 (v_texcoord.x * width, v_texcoord.y * height);
    vec2 cell = floor (px / 12.0);
    float r = hash (cell);
    float speck = r > 1.0 - (density / 100.0) * 0.5 ? 1.0 : 0.0;
    vec2 centre = (cell + 0.5) * 12.0;
    float d = length (px - centre);
    float size = 1.0 + hash (cell + 4.0) * 2.0;
    float dot_spec = speck * (1.0 - smoothstep (size * 0.4, size, d));
    float darken = 1.0 - speck * 0.25;
    vec3 result = c.rgb * darken + vec3 (0.85, 0.85, 0.85) * dot_spec * 0.5;
    gl_FragColor = vec4 (mix (c.rgb, result, strength / 100.0), c.a);
}
