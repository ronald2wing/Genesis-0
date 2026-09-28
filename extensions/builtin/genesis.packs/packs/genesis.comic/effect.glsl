// SPDX-License-Identifier: GPL-3.0-or-later
// SPDX-FileCopyrightText: 2026 Genesis-0 contributors
//
// The fragment BODY; see genesis.exposure/effect.glsl for the contract.

uniform float width;
uniform float height;

float lum (vec3 c) {
    return dot (c, vec3 (0.2126, 0.7152, 0.0722));
}

void main () {
    vec4 c = texture2D (tex, v_texcoord);
    float l = lum (c.rgb);
    vec3 poster = floor (c.rgb * levels + 0.5) / levels;
    vec2 px = vec2 (1.0 / width, 1.0 / height);
    float lx = lum (texture2D (tex, v_texcoord + vec2 (px.x, 0.0)).rgb);
    float ly = lum (texture2D (tex, v_texcoord + vec2 (0.0, px.y)).rgb);
    float grad = length (vec2 (l - lx, l - ly));
    float ink = 1.0 - smoothstep (0.02, 0.18, grad);
    vec3 outlined = poster * mix (1.0, ink, edge / 100.0);
    gl_FragColor = vec4 (mix (c.rgb, outlined, strength / 100.0), c.a);
}
