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
// The highlights' light, weighed by how far their display level is past 0.55
// (the knee of the bloom) and taken to the 2.4th power, spread by a Gaussian
// of twelve of the layer's pixels, and screened back over the picture by the
// amount as it breathes. The original's across/down passes fold into one 9x9
// kernel over tex; the original's time-driven `speed` becomes a keyframed
// `pulse` the host animates (no time uniform exists here).

uniform float width;
uniform float height;

void main () {
    vec4 c = texture2D (tex, v_texcoord);
    vec2 t = vec2 (1.0 / width, 1.0 / height) * 12.0;
    vec3 sum = vec3 (0.0);
    float total = 0.0;
    for (int y = -4; y <= 4; y++) {
        for (int x = -4; x <= 4; x++) {
            vec3 s = texture2D (tex, v_texcoord + vec2 (float (x), float (y)) * t).rgb;
            float bright = pow (smoothstep (0.55, 1.0, dot (s, vec3 (0.2126, 0.7152, 0.0722))), 2.4);
            float w = exp (-float (x * x + y * y) / 2.0);
            sum += s * bright * w;
            total += w;
        }
    }
    vec3 glow = clamp (sum / total * (amount / 100.0) * pulse, 0.0, 1.0);
    gl_FragColor = vec4 (1.0 - (1.0 - c.rgb) * (1.0 - glow), c.a);
}
