// SPDX-License-Identifier: GPL-3.0-or-later
// SPDX-FileCopyrightText: 2026 Genesis-0 contributors
//
// The fragment BODY; see genesis.exposure/effect.glsl for the contract.
// `width` is set by glshader every frame, not by the pack.

uniform float width;

float hash (vec2 p) {
    return fract (sin (dot (p, vec2 (127.1, 311.7))) * 43758.5453);
}

void main () {
    vec2 uv = v_texcoord;
    float s = strength / 100.0;
    float band = floor (uv.y * 20.0);
    float rnd = hash (vec2 (band, 1.0));
    uv.x += step (0.9, rnd) * (rnd - 0.5) * s * 0.3;
    float split = s * 6.0 / width;
    float r = texture2D (tex, uv + vec2 (split, 0.0)).r;
    float g = texture2D (tex, uv).g;
    float b = texture2D (tex, uv - vec2 (split, 0.0)).b;
    gl_FragColor = vec4 (r, g, b, 1.0);
}
