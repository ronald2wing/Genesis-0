// SPDX-License-Identifier: GPL-3.0-or-later
// SPDX-FileCopyrightText: 2026 Genesis-0 contributors
//
// The fragment BODY; see genesis.exposure/effect.glsl for the contract.

uniform float width;
uniform float height;

void main () {
    vec4 c = texture2D (tex, v_texcoord);
    vec2 cell = vec2 (tile_size / width, tile_size / height);
    vec2 sample = (floor (v_texcoord / cell) + 0.5) * cell;
    vec3 flat = texture2D (tex, clamp (sample, 0.0, 1.0)).rgb;
    gl_FragColor = vec4 (mix (c.rgb, flat, strength / 100.0), c.a);
}
