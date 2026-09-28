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
    vec2 cell = vec2 (facet_size / width, facet_size / height);
    vec2 ij = floor (v_texcoord / cell);
    vec2 jitter_off = (vec2 (hash (ij), hash (ij + 5.9)) - 0.5) * cell * (jitter / 50.0);
    vec2 sample = (ij + 0.5) * cell + jitter_off;
    float shade = 0.9 + 0.2 * hash (ij + 11.7);
    vec3 facet = texture2D (tex, clamp (sample, 0.0, 1.0)).rgb * shade;
    gl_FragColor = vec4 (mix (c.rgb, facet, strength / 100.0), c.a);
}
