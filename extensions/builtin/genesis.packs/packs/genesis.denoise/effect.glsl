// SPDX-License-Identifier: GPL-3.0-or-later
// SPDX-FileCopyrightText: 2026 Genesis-0 contributors
//
// The fragment BODY; see genesis.exposure/effect.glsl for the contract.
// A bilateral filter: each neighbour is weighted by how far away it is and
// how close its colour is, so flat areas smooth while edges stay. `width`
// and `height` are set by glshader every frame, not by the pack.

uniform float width;
uniform float height;

void main () {
    vec2 t = vec2 (1.0 / width, 1.0 / height) * radius;
    vec3 c = texture2D (tex, v_texcoord).rgb;
    float sigma = 0.05 + 0.4 * strength / 100.0;
    float inv = 1.0 / (2.0 * sigma * sigma);
    vec3 sum = vec3 (0.0);
    float total = 0.0;
    for (int y = -2; y <= 2; y++) {
        for (int x = -2; x <= 2; x++) {
            vec3 s = texture2D (tex, v_texcoord + vec2 (float (x), float (y)) * t).rgb;
            float spatial = exp (-float (x * x + y * y) / 2.0);
            float d = dot (s - c, s - c);
            float range = exp (-d * inv);
            float w = spatial * range;
            sum += s * w;
            total += w;
        }
    }
    gl_FragColor = vec4 (sum / max (total, 0.0001), 1.0);
}
