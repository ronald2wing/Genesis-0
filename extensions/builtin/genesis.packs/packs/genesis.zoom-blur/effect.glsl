// SPDX-License-Identifier: GPL-3.0-or-later
// SPDX-FileCopyrightText: 2026 Genesis-0 contributors
//
// Re-authored from Concat's concat-effects package `concat.zoom-blur`,
// SPDX-License-Identifier: GPL-3.0-or-later,
// SPDX-FileCopyrightText: 2026 Jareer and Concat contributors.
//
// The fragment BODY for the glshader route; see genesis.exposure/effect.glsl
// for the contract. The engine's glshader prepends its prelude (precision,
// the varying, and the one texture `tex` it binds) and this pack's parameter
// uniform declarations, then appends this file. The body must not declare a
// sampler (the prelude owns `tex`) nor redeclare a parameter uniform.
//
// Each pixel streaked along the line to the centre, over `amount` of its
// distance from it: the picture drawn at every scale from its own down to
// `amount` smaller about the centre, at a steady rate of zoom, and averaged -
// nothing at the centre, the most at the edge. The original's three passes of
// sixteen scales each fold into one loop of thirty-two scales straight from
// tex; no `width`/`height` is needed because the scales are in uv.

void main () {
    vec2 centre = vec2 (x, y) / 100.0;
    float span = -log (1.0 - clamp (amount / 100.0, 0.0, 0.99));
    vec4 sum = vec4 (0.0);
    for (int i = 0; i < 32; i++) {
        float by = span * float (i) / 32.0;
        vec2 at = centre + (v_texcoord - centre) * exp (-by);
        sum += texture2D (tex, at);
    }
    gl_FragColor = sum / 32.0;
}
