// SPDX-License-Identifier: GPL-3.0-or-later
// SPDX-FileCopyrightText: 2026 Genesis-0 contributors
//
// The fragment BODY; see genesis.exposure/effect.glsl for the contract.
// `levels` is an int parameter, laid out as a float uniform by the resolver.
// `width` and `height` are set by glshader every frame, not by the pack.

uniform float width;
uniform float height;

float bayer2 (vec2 p) {
    p = mod (floor (p), 2.0);
    return 2.0 * p.x + 3.0 * p.y - 4.0 * p.x * p.y;
}

float bayer4 (vec2 p) {
    vec2 lo = mod (floor (p), 2.0);
    vec2 hi = mod (floor (p / 2.0), 2.0);
    return 4.0 * bayer2 (lo) + bayer2 (hi);
}

void main () {
    vec4 c = texture2D (tex, v_texcoord);
    float l = dot (c.rgb, vec3 (0.2126, 0.7152, 0.0722));
    vec2 px = v_texcoord * vec2 (width, height);
    float th = (bayer4 (px) + 0.5) / 16.0 - 0.5;
    float steps = max (floor (levels), 2.0) - 1.0;
    float q = clamp (floor (l * steps + th + 0.5) / steps, 0.0, 1.0);
    gl_FragColor = vec4 (vec3 (q), c.a);
}
