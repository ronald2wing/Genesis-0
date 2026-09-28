// SPDX-License-Identifier: GPL-3.0-or-later
// SPDX-FileCopyrightText: 2026 Genesis-0 contributors
//
// The fragment BODY; see genesis.exposure/effect.glsl for the contract.

void main () {
    vec2 uv = v_texcoord - vec2 (0.5);
    float a = radians (angle);
    vec2 dir = vec2 (cos (a), sin (a));
    vec2 perp = vec2 (-dir.y, dir.x);
    float along = dot (uv, dir);
    float side = dot (uv, perp);
    float s = strength / 100.0;
    float streak = exp (-side * side * 400.0) * exp (-along * along * 2.0);
    float spot = exp (-dot (uv, uv) * 20.0);
    vec3 c = texture2D (tex, v_texcoord).rgb;
    vec3 flare = (streak * 1.2 + spot * 0.8) * s;
    gl_FragColor = vec4 (c + flare, 1.0);
}
