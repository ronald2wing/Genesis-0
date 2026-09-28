// SPDX-License-Identifier: GPL-3.0-or-later
// SPDX-FileCopyrightText: 2026 Genesis-0 contributors
//
// The fragment BODY; see genesis.exposure/effect.glsl for the contract.
// `width` and `height` are set by glshader every frame, not by the pack.

uniform float width;
uniform float height;

void main () {
    // Work in aspect-corrected space so the bulge stays round, not elliptical.
    vec2 aspect = vec2 (width / height, 1.0);
    vec2 p = (v_texcoord - 0.5) * aspect;
    float k = distortion / 100.0 * 0.6;
    float f = 1.0 + k * dot (p, p);
    vec2 uv = p * f / aspect + 0.5;
    if (uv.x < 0.0 || uv.x > 1.0 || uv.y < 0.0 || uv.y > 1.0) {
        gl_FragColor = vec4 (0.0);
        return;
    }
    gl_FragColor = texture2D (tex, uv);
}
