// SPDX-License-Identifier: GPL-3.0-or-later
// SPDX-FileCopyrightText: 2026 Genesis-0 contributors
//
// The fragment BODY; see genesis.exposure/effect.glsl for the contract.
// `segments` is an int parameter, laid out as a float uniform by the resolver.

void main () {
    vec2 uv = v_texcoord - vec2 (0.5);
    float a = atan (uv.y, uv.x) + radians (angle);
    float n = max (floor (segments + 0.5), 3.0);
    float wedge = 3.14159265 / n;
    a = mod (a, 2.0 * wedge);
    a = abs (a - wedge);
    float r = length (uv) * 1.4142;
    vec2 k = vec2 (cos (a), sin (a)) * r + vec2 (0.5);
    gl_FragColor = texture2D (tex, k);
}
