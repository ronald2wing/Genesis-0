// SPDX-License-Identifier: GPL-3.0-or-later
// SPDX-FileCopyrightText: 2026 Genesis-0 contributors
//
// The fragment BODY; see genesis.exposure/effect.glsl for the contract.

uniform float width;
uniform float height;

float hash (vec2 p) {
    return fract (sin (dot (p, vec2 (127.1, 311.7))) * 43758.5453123);
}

float noise (vec2 p) {
    vec2 i = floor (p);
    vec2 f = fract (p);
    f = f * f * (3.0 - 2.0 * f);
    return mix (
        mix (hash (i), hash (i + vec2 (1.0, 0.0)), f.x),
        mix (hash (i + vec2 (0.0, 1.0)), hash (i + vec2 (1.0, 1.0)), f.x),
        f.y);
}

void main () {
    vec4 c = texture2D (tex, v_texcoord);
    vec2 p = vec2 (v_texcoord.x * width / 240.0, v_texcoord.y * height / 240.0);
    float n = noise (p) * 0.65 + noise (p * 2.7) * 0.35;
    float blotch = smoothstep (0.55, 0.9, n) * (density / 100.0);
    vec3 dirty = c.rgb * (1.0 - blotch * 0.6);
    gl_FragColor = vec4 (mix (c.rgb, dirty, strength / 100.0), c.a);
}
