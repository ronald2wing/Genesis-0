// SPDX-License-Identifier: GPL-3.0-or-later
// SPDX-FileCopyrightText: 2026 Genesis-0 contributors
//
// Re-authored from Concat's concat-effects package `concat.gaussian-blur`,
// SPDX-License-Identifier: GPL-3.0-or-later,
// SPDX-FileCopyrightText: 2026 Jareer and Concat contributors.
//
// The fragment BODY for the glshader route; see genesis.exposure/effect.glsl
// for the contract. The engine's glshader prepends its prelude (precision,
// the varying, and the one texture `tex` it binds) and this pack's parameter
// uniform declarations, then appends this file. The body must not declare a
// sampler (the prelude owns `tex`) nor redeclare a parameter uniform. `width`
// and `height` are set by glshader every frame, not by the pack.
//
// The original blurred across then down a named intermediate (a separable
// Gaussian); this body folds both axes into one 9x9 kernel over tex, so no
// second distinct input is needed. A 2-D Gaussian is the product of the two
// axes, and `radius` pixels is the sigma of each, so the taps here are the
// product of the original's two passes.

uniform float width;
uniform float height;

void main () {
    vec2 t = vec2 (1.0 / width, 1.0 / height) * radius;
    vec4 sum = vec4 (0.0);
    float total = 0.0;
    for (int y = -4; y <= 4; y++) {
        for (int x = -4; x <= 4; x++) {
            float w = exp (-float (x * x + y * y) / 2.0);
            sum += texture2D (tex, v_texcoord + vec2 (float (x), float (y)) * t) * w;
            total += w;
        }
    }
    gl_FragColor = sum / total;
}
