// SPDX-License-Identifier: GPL-3.0-or-later
// SPDX-FileCopyrightText: 2026 Genesis-0 contributors
//
// Re-authored from Concat's concat-effects package `concat.film-grain`,
// SPDX-License-Identifier: GPL-3.0-or-later,
// SPDX-FileCopyrightText: 2026 Jareer and Concat contributors.
//
// The fragment BODY; see genesis.exposure/effect.glsl for the contract.
// Grain hashed per pixel, held still: the GL path has no time uniform, so
// the grain does not dance frame to frame. `width` and `height` are set by
// glshader every frame, not by the pack.

uniform float width;
uniform float height;

float hash (vec2 p) {
    return fract (sin (dot (p, vec2 (127.1, 311.7))) * 43758.5453);
}

void main () {
    vec4 c = texture2D (tex, v_texcoord);
    vec2 px = floor (v_texcoord * vec2 (width, height));
    float n = hash (px) - 0.5;
    gl_FragColor = vec4 (c.rgb + vec3 (n * (amount / 100.0) * 0.2), c.a);
}
