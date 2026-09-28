// SPDX-License-Identifier: GPL-3.0-or-later
// SPDX-FileCopyrightText: 2026 Genesis-0 contributors
//
// The fragment BODY; see genesis.exposure/effect.glsl for the contract.

void main () {
    vec2 uv = v_texcoord - vec2 (0.5);
    float a = radians (angle);
    vec2 dir = vec2 (cos (a), sin (a));
    float d = dot (uv, dir);
    float s = strength / 100.0;
    float leak = smoothstep (0.0, 0.6, d + 0.5) * s;
    vec3 tint = vec3 (1.0, 0.55, 0.2);
    if (color < 0.5) {
        tint = vec3 (1.0, 0.55, 0.2);
    } else if (color < 1.5) {
        tint = vec3 (0.3, 0.6, 1.0);
    } else {
        tint = vec3 (0.25, 1.0, 0.4);
    }
    vec3 c = texture2D (tex, v_texcoord).rgb;
    gl_FragColor = vec4 (c + tint * leak, 1.0);
}
