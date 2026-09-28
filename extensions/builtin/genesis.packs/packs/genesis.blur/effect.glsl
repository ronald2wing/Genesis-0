// SPDX-License-Identifier: GPL-3.0-or-later
// SPDX-FileCopyrightText: 2026 Genesis-0 contributors
//
// Re-authored from Concat's concat-effects package `concat.gaussian-blur`,
// SPDX-License-Identifier: GPL-3.0-or-later,
// SPDX-FileCopyrightText: 2026 Jareer and Concat contributors.
//
// The fragment BODY; see genesis.exposure/effect.glsl for the contract.
// A 5x5 Gaussian folded into one pass: the multi-pass `across`/`down`
// intermediates are not expressible on the GL path, so the blur is the
// product of the two weighed here. `width` and `height` are set by glshader.

uniform float width;
uniform float height;

void main () {
    vec2 t = vec2 (1.0 / width, 1.0 / height) * radius;
    vec4 sum = vec4 (0.0);
    float total = 0.0;
    for (int y = -2; y <= 2; y++) {
        for (int x = -2; x <= 2; x++) {
            float w = exp (-float (x * x + y * y) / 2.0);
            sum += texture2D (tex, v_texcoord + vec2 (float (x), float (y)) * t) * w;
            total += w;
        }
    }
    gl_FragColor = sum / total;
}
