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
    float l = dot (c.rgb, vec3 (0.2126, 0.7152, 0.0722));
    vec2 cell = vec2 (dot_size / width, dot_size / height);
    vec2 ij = floor (v_texcoord / cell);
    float threshold = l;
    float r = hash (ij);
    float dot_here = 1.0 - smoothstep (r - 0.06, r + 0.06, threshold);
    vec3 ink = vec3 (0.0);
    vec3 paper = vec3 (0.96, 0.94, 0.9);
    vec3 result = mix (paper, ink, dot_here);
    gl_FragColor = vec4 (mix (c.rgb, result, strength / 100.0), c.a);
}
