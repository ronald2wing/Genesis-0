// SPDX-License-Identifier: GPL-3.0-or-later
// SPDX-FileCopyrightText: 2026 Genesis-0 contributors
//
// Re-authored from Concat's concat-effects package `concat.glow`,
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
// The picture's light spread wide - a Gaussian of sixteen of the layer's
// pixels, the original's across/down passes folded into one 9x9 kernel over
// tex - and screened back by the amount. The screen is the whole picture's,
// not a bright-pass: the glow reads on every pixel, bright or not.

uniform float width;
uniform float height;

void main () {
    vec4 c = texture2D (tex, v_texcoord);
    vec2 t = vec2 (1.0 / width, 1.0 / height) * 16.0;
    vec3 sum = vec3 (0.0);
    float total = 0.0;
    for (int y = -4; y <= 4; y++) {
        for (int x = -4; x <= 4; x++) {
            float w = exp (-float (x * x + y * y) / 2.0);
            sum += texture2D (tex, v_texcoord + vec2 (float (x), float (y)) * t).rgb * w;
            total += w;
        }
    }
    vec3 glow = sum / total;
    vec3 screened = 1.0 - (1.0 - c.rgb) * (1.0 - glow);
    gl_FragColor = vec4 (mix (c.rgb, screened, amount / 100.0), c.a);
}
