// SPDX-License-Identifier: GPL-3.0-or-later
// SPDX-FileCopyrightText: 2026 Genesis-0 contributors
//
// The fragment BODY; see genesis.exposure/effect.glsl for the contract.
// Nine taps spaced along the direction `angle`, spread over `radius` pixels.
// `width` and `height` are set by glshader every frame, not by the pack.

uniform float width;
uniform float height;

void main () {
    float a = radians (angle);
    vec2 dir = vec2 (cos (a), sin (a));
    vec2 step = dir * vec2 (1.0 / width, 1.0 / height) * (radius / 4.0);
    vec4 sum = vec4 (0.0);
    for (int i = -4; i <= 4; i++) {
        sum += texture2D (tex, v_texcoord + step * float (i));
    }
    gl_FragColor = sum / 9.0;
}
