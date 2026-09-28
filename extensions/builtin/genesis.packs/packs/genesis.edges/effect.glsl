// SPDX-License-Identifier: GPL-3.0-or-later
// SPDX-FileCopyrightText: 2026 Genesis-0 contributors
//
// The fragment BODY; see genesis.exposure/effect.glsl for the contract.
// `width` and `height` are set by glshader every frame, not by the pack.

uniform float width;
uniform float height;

float lum (vec2 uv) {
    return dot (texture2D (tex, uv).rgb, vec3 (0.2126, 0.7152, 0.0722));
}

void main () {
    vec2 t = vec2 (1.0 / width, 1.0 / height);
    float tl = lum (v_texcoord + vec2 (-t.x, -t.y));
    float tm = lum (v_texcoord + vec2 (0.0, -t.y));
    float tr = lum (v_texcoord + vec2 (t.x, -t.y));
    float ml = lum (v_texcoord + vec2 (-t.x, 0.0));
    float mr = lum (v_texcoord + vec2 (t.x, 0.0));
    float bl = lum (v_texcoord + vec2 (-t.x, t.y));
    float bm = lum (v_texcoord + vec2 (0.0, t.y));
    float br = lum (v_texcoord + vec2 (t.x, t.y));
    float gx = (tr + 2.0 * mr + br) - (tl + 2.0 * ml + bl);
    float gy = (bl + 2.0 * bm + br) - (tl + 2.0 * tm + tr);
    float e = length (vec2 (gx, gy));
    float s = strength / 100.0;
    gl_FragColor = vec4 (vec3 (mix (1.0, 1.0 - clamp (e, 0.0, 1.0), s)), 1.0);
}
