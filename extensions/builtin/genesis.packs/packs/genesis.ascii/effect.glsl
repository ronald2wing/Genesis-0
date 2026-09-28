// SPDX-License-Identifier: GPL-3.0-or-later
// SPDX-FileCopyrightText: 2026 Genesis-0 contributors
//
// The fragment BODY; see genesis.exposure/effect.glsl for the contract.
// `width` and `height` are set by glshader every frame, not by the pack.

uniform float width;
uniform float height;

void main () {
    float cols = max (floor (columns + 0.5), 4.0);
    float rows = cols * height / width;
    vec2 cell = floor (v_texcoord * vec2 (cols, rows));
    vec2 uv = fract (v_texcoord * vec2 (cols, rows));
    vec2 base = (cell + 0.5) / vec2 (cols, rows);
    vec2 t = vec2 (1.0 / width, 1.0 / height);
    float l = 0.0;
    for (int y = -2; y <= 2; y++) {
        for (int x = -2; x <= 2; x++) {
            l += dot (texture2D (tex, base + vec2 (float (x), float (y)) * t).rgb, vec3 (0.2126, 0.7152, 0.0722));
        }
    }
    l /= 25.0;
    float level = floor (l * 8.0) / 8.0;
    float ink = step (1.0 - level, uv.y);
    float border = clamp (step (0.95, uv.x) + step (0.95, uv.y), 0.0, 1.0);
    vec3 col = mix (vec3 (0.0), vec3 (1.0), ink);
    gl_FragColor = vec4 (mix (col, vec3 (0.0), border * 0.6), 1.0);
}
