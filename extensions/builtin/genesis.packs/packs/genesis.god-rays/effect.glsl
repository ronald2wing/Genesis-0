// SPDX-License-Identifier: GPL-3.0-or-later
// SPDX-FileCopyrightText: 2026 Genesis-0 contributors
//
// The fragment BODY; see genesis.exposure/effect.glsl for the contract.

void main () {
    vec2 uv = v_texcoord - vec2 (0.5);
    float a = atan (uv.y, uv.x);
    float base = radians (angle);
    float diff = abs (sin (a - base));
    float s = strength / 100.0;
    float ray = exp (-diff * diff * (10.0 + 40.0 * s) * (0.2 + length (uv)));
    vec3 c = texture2D (tex, v_texcoord).rgb;
    gl_FragColor = vec4 (c * (1.0 + ray * s * 2.0), 1.0);
}
