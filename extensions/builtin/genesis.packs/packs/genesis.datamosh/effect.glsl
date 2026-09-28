// SPDX-License-Identifier: GPL-3.0-or-later
// SPDX-FileCopyrightText: 2026 Genesis-0 contributors
//
// The fragment BODY; see genesis.exposure/effect.glsl for the contract.

float hash (float x) {
    return fract (sin (x) * 43758.5453123);
}

void main () {
    vec4 c = texture2D (tex, v_texcoord);
    float rows = floor (float (blocks));
    float row = floor (v_texcoord.y * rows);
    float offset = (hash (row) - 0.5) * (displacement / 100.0) * 0.5;
    vec3 shifted = texture2D (tex, vec2 (fract (v_texcoord.x + offset), v_texcoord.y)).rgb;
    gl_FragColor = vec4 (mix (c.rgb, shifted, strength / 100.0), c.a);
}
