// SPDX-License-Identifier: GPL-3.0-or-later
// SPDX-FileCopyrightText: 2026 Genesis-0 contributors
//
// The fragment BODY; see genesis.exposure/effect.glsl for the contract.

vec3 hue_rgb (float h) {
    return clamp (abs (mod (h * 6.0 + vec3 (0.0, 4.0, 2.0), 6.0) - 3.0) - 1.0, 0.0, 1.0);
}

void main () {
    vec4 c = texture2D (tex, v_texcoord);
    float t = thickness / 100.0;
    float edge = step (1.0 - t, v_texcoord.x) + step (1.0 - t, v_texcoord.y)
        + step (v_texcoord.x, t) + step (v_texcoord.y, t);
    float inside = step (0.5, edge);
    vec3 colour = hue_rgb (hue / 360.0);
    vec3 result = mix (c.rgb, colour, inside);
    gl_FragColor = vec4 (mix (c.rgb, result, strength / 100.0), c.a);
}
