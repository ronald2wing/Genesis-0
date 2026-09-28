// SPDX-License-Identifier: GPL-3.0-or-later
// SPDX-FileCopyrightText: 2026 Genesis-0 contributors
//
// The fragment BODY; see genesis.exposure/effect.glsl for the contract.
// `width` and `height` are set by glshader every frame, not by the pack.

uniform float width;
uniform float height;

void main () {
    vec2 t = vec2 (1.0 / width, 1.0 / height);
    vec4 c = texture2D (tex, v_texcoord);
    float d = abs (v_texcoord.y - focus_y);
    float b = max (band, 0.02);
    float m = smoothstep (b * 0.5, b, d);
    vec4 sum = vec4 (0.0);
    float total = 0.0;
    for (int y = -6; y <= 6; y++) {
        for (int x = -6; x <= 6; x++) {
            float w = exp (-float (x * x + y * y) / 18.0);
            sum += texture2D (tex, v_texcoord + vec2 (float (x), float (y)) * t * amount) * w;
            total += w;
        }
    }
    vec4 blurred = sum / total;
    gl_FragColor = mix (c, blurred, m);
}
