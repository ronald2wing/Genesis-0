// SPDX-License-Identifier: GPL-3.0-or-later
// SPDX-FileCopyrightText: 2026 Genesis-0 contributors
//
// The fragment BODY; see genesis.exposure/effect.glsl for the contract.

uniform float width;
uniform float height;

float lum (vec3 c) {
    return dot (c, vec3 (0.2126, 0.7152, 0.0722));
}

vec3 hue_rgb (float h) {
    return clamp (abs (mod (h * 6.0 + vec3 (0.0, 4.0, 2.0), 6.0) - 3.0) - 1.0, 0.0, 1.0);
}

void main () {
    vec4 c = texture2D (tex, v_texcoord);
    float e = edge / max (width, 1.0);
    float l = lum (c.rgb);
    float lx0 = lum (texture2D (tex, v_texcoord + vec2 (-e, 0.0)).rgb);
    float lx1 = lum (texture2D (tex, v_texcoord + vec2 (e, 0.0)).rgb);
    float ly0 = lum (texture2D (tex, v_texcoord + vec2 (0.0, -e)).rgb);
    float ly1 = lum (texture2D (tex, v_texcoord + vec2 (0.0, e)).rgb);
    float grad = abs (l - lx0) + abs (l - lx1) + abs (l - ly0) + abs (l - ly1);
    float glow = smoothstep (0.02, 0.3, grad);
    vec3 neon = hue_rgb (hue / 360.0);
    vec3 result = c.rgb * 0.25 + neon * glow * 1.2;
    gl_FragColor = vec4 (mix (c.rgb, result, strength / 100.0), c.a);
}
