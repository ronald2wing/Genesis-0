// SPDX-License-Identifier: GPL-3.0-or-later
// SPDX-FileCopyrightText: 2026 Genesis-0 contributors
//
// The fragment BODY; see genesis.exposure/effect.glsl for the contract.

float hash (vec2 p) {
    return fract (sin (dot (p, vec2 (127.1, 311.7))) * 43758.5453123);
}

void main () {
    vec4 c = texture2D (tex, v_texcoord);
    float centre = band_y / 100.0;
    float half_h = band_height / 200.0;
    float dist = abs (v_texcoord.y - centre);
    float band = 1.0 - smoothstep (half_h * 0.5, half_h, dist);
    vec2 p = v_texcoord;
    p.x += (hash (vec2 (floor (v_texcoord.y * 200.0), 3.0)) - 0.5) * band * 0.15;
    vec3 glitched = texture2D (tex, clamp (p, 0.0, 1.0)).rgb;
    glitched.r = mix (glitched.r, texture2D (tex, clamp (p + vec2 (0.01, 0.0), 0.0, 1.0)).r, band);
    glitched.b = mix (glitched.b, texture2D (tex, clamp (p - vec2 (0.01, 0.0), 0.0, 1.0)).b, band);
    float snow = hash (v_texcoord * vec2 (700.0, 3.0)) * band;
    vec3 result = mix (glitched, vec3 (snow), band * 0.4);
    gl_FragColor = vec4 (mix (c.rgb, result, strength / 100.0), c.a);
}
