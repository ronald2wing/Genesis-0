// SPDX-License-Identifier: GPL-3.0-or-later
// SPDX-FileCopyrightText: 2026 Genesis-0 contributors
//
// The fragment BODY for the glshader route; see genesis.exposure/effect.glsl
// for the contract. The engine's glshader prepends its prelude (precision,
// the varying, and the one texture `tex` it binds) and this pack's parameter
// uniform declarations, then appends this file. The body must not declare a
// sampler (the prelude owns `tex`) nor redeclare a parameter uniform. `width`
// and `height` are set by glshader every frame, not by the pack.
//
// A streak `length` pixels along a line `angle` degrees from level -
// anticlockwise, as a protractor reads it - in light. The original's two
// passes (a coarse Gaussian, then straight lines between its samples) fold
// into one triangle-weighted streak sampled straight from tex: the weight
// falls from one at the centre to nothing at either end, which is the
// straight lines the original drew.

uniform float width;
uniform float height;

void main () {
    float a = radians (angle);
    vec2 dir = vec2 (cos (a), -sin (a));
    vec2 t = dir * (length / 2.0) / vec2 (width, height);
    vec4 sum = vec4 (0.0);
    float total = 0.0;
    for (int i = -32; i <= 32; i++) {
        float w = 1.0 - abs (float (i)) / 32.0;
        sum += texture2D (tex, v_texcoord + t * (float (i) / 32.0)) * w;
        total += w;
    }
    gl_FragColor = sum / total;
}
