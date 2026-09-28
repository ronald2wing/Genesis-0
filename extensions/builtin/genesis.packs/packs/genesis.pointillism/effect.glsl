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
    vec2 cell = vec2 (dot_size / width, dot_size / height);
    vec2 ij = floor (v_texcoord / cell);
    vec2 centre = (ij + 0.5) * cell;
    vec2 jitter = (vec2 (hash (ij), hash (ij + 7.3)) - 0.5) * cell * 0.8;
    vec4 spot = texture2D (tex, clamp (centre + jitter, 0.0, 1.0));
    float d = length (v_texcoord - (centre + jitter));
    float r = cell.x * 0.5 * (0.6 + 0.4 * hash (ij + 3.1));
    float inside = 1.0 - smoothstep (r * 0.85, r, d);
    vec3 dotted = mix (c.rgb * 0.9, spot.rgb, inside);
    gl_FragColor = vec4 (mix (c.rgb, dotted, strength / 100.0), c.a);
}
