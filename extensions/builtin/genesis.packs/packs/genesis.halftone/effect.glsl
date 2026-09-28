// SPDX-License-Identifier: GPL-3.0-or-later
// SPDX-FileCopyrightText: 2026 Genesis-0 contributors
//
// Re-authored from Concat's concat-effects package `concat.halftone`,
// SPDX-License-Identifier: GPL-3.0-or-later,
// SPDX-FileCopyrightText: 2026 Jareer and Concat contributors.
//
// The fragment BODY; see genesis.exposure/effect.glsl for the contract.
// `width` and `height` are set by glshader every frame, not by the pack.

uniform float width;
uniform float height;

void main () {
    vec4 c = texture2D (tex, v_texcoord);
    vec2 at = v_texcoord * vec2 (width, height);
    float cell = max (size, 2.0);
    vec2 middle = (floor (at / cell) + vec2 (0.5)) * cell;
    float seen = dot (texture2D (tex, middle / vec2 (width, height)).rgb, vec3 (0.2126, 0.7152, 0.0722));
    float level = clamp (seen, 0.0, 1.0);
    float radius = sqrt (1.0 - level) * cell * 0.7071;
    float ink = 1.0 - smoothstep (radius - 0.75, radius + 0.75, distance (at, middle));
    gl_FragColor = vec4 (vec3 ((1.0 - ink) * max (seen, 1.0)), c.a);
}
