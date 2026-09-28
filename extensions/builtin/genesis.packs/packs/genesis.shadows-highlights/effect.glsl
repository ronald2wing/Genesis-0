// SPDX-License-Identifier: GPL-3.0-or-later
// SPDX-FileCopyrightText: 2026 Genesis-0 contributors
//
// The fragment BODY; see genesis.exposure/effect.glsl for the contract.
// The shadow and highlight sliders act on opposite ends of the luminance
// range, weighted by how far a pixel sits in that end, so the middle is
// left alone.

void main () {
    vec4 c = texture2D (tex, v_texcoord);
    vec3 rgb = c.rgb;
    float l = dot (rgb, vec3 (0.2126, 0.7152, 0.0722));
    float w_shadow = 1.0 - smoothstep (0.0, 0.6, l);
    float w_highlight = smoothstep (0.4, 1.0, l);
    rgb = rgb
        + vec3 (shadows / 100.0 * 0.5) * w_shadow
        + vec3 (highlights / 100.0 * 0.5) * w_highlight;
    gl_FragColor = vec4 (clamp (rgb, 0.0, 1.0), c.a);
}
