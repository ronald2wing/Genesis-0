// SPDX-License-Identifier: GPL-3.0-or-later
// SPDX-FileCopyrightText: 2026 Genesis-0 contributors
//
// The fragment BODY; see genesis.exposure/effect.glsl for the contract.
// `width` and `height` are set by glshader every frame, not by the pack.
// `radius` is an int parameter, laid out as a float uniform by the resolver.

uniform float width;
uniform float height;

float lum (vec2 uv) {
    return dot (texture2D (tex, uv).rgb, vec3 (0.2126, 0.7152, 0.0722));
}

void main () {
    vec4 c = texture2D (tex, v_texcoord);
    vec2 t = vec2 (1.0 / width, 1.0 / height);
    float r = max (floor (radius + 0.5), 1.0);
    vec2 off = t * r;

    // Sobel edges sampled across the pencil's tip width.
    float tl = lum (v_texcoord + vec2 (-off.x, -off.y));
    float tm = lum (v_texcoord + vec2 (0.0, -off.y));
    float tr = lum (v_texcoord + vec2 (off.x, -off.y));
    float ml = lum (v_texcoord + vec2 (-off.x, 0.0));
    float mr = lum (v_texcoord + vec2 (off.x, 0.0));
    float bl = lum (v_texcoord + vec2 (-off.x, off.y));
    float bm = lum (v_texcoord + vec2 (0.0, off.y));
    float br = lum (v_texcoord + vec2 (off.x, off.y));
    float gx = (tr + 2.0 * mr + br) - (tl + 2.0 * ml + bl);
    float gy = (bl + 2.0 * bm + br) - (tl + 2.0 * tm + tr);
    float e = clamp (length (vec2 (gx, gy)), 0.0, 1.0);

    float l = dot (c.rgb, vec3 (0.2126, 0.7152, 0.0722));
    vec3 paper = vec3 (0.96, 0.95, 0.92);
    vec3 ink = vec3 (0.08, 0.08, 0.10);
    vec3 sketch = mix (paper, ink, e);
    // Keep a hint of the picture's own shading under the linework.
    vec3 shaded = mix (sketch, vec3 (l), 0.35);

    float s = strength / 100.0;
    gl_FragColor = vec4 (mix (c.rgb, shaded, s), c.a);
}
