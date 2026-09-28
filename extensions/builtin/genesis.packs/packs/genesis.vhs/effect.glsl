// SPDX-License-Identifier: GPL-3.0-or-later
// SPDX-FileCopyrightText: 2026 Genesis-0 contributors
//
// The fragment BODY; see genesis.exposure/effect.glsl for the contract.
// `width` and `height` are set by glshader every frame, not by the pack.

uniform float width;
uniform float height;

float hash (vec2 p) {
    return fract (sin (dot (p, vec2 (127.1, 311.7))) * 43758.5453);
}

void main () {
    vec2 uv = v_texcoord;
    float s = strength / 100.0;
    float tr = tracking / 100.0;

    // Tracking: whole bands jump sideways, keyed by an 8-row hash.
    float band = floor (uv.y * height / 8.0);
    float rnd = hash (vec2 (band, 3.0));
    uv.x = fract (uv.x + step (0.82, rnd) * (rnd - 0.5) * tr * 0.06);

    // Chromatic fringing, wider as the tape wobbles harder.
    float split = s * 4.0 / width + tr * 0.002;
    float r = texture2D (tex, uv + vec2 (split, 0.0)).r;
    float g = texture2D (tex, uv).g;
    float b = texture2D (tex, uv - vec2 (split, 0.0)).b;
    vec3 c = vec3 (r, g, b);

    // A tired tape drains a little colour.
    float l = dot (c, vec3 (0.2126, 0.7152, 0.0722));
    c = mix (c, vec3 (l), 0.25 * s);

    // Scanlines darken every other row.
    float sl = 0.92 + 0.08 * sin (uv.y * height * 3.14159);
    c *= mix (1.0, sl, 0.5 * s);

    // Per-pixel noise, held still (the GL path has no time uniform).
    vec2 px = floor (uv * vec2 (width, height));
    c += (hash (px + vec2 (1.0, 1.0)) - 0.5) * 0.06 * s;

    gl_FragColor = vec4 (c, 1.0);
}
