// SPDX-License-Identifier: GPL-3.0-or-later
// SPDX-FileCopyrightText: 2026 Genesis-0 contributors
//
// The fragment BODY; see genesis.exposure/effect.glsl for the contract.
// The ACES filmic approximation: a smooth S-curve that softens the shadows
// and rolls off the highlights, mixed back by strength.

void main () {
    vec4 c = texture2D (tex, v_texcoord);
    vec3 x = clamp (c.rgb, 0.0, 1.0);
    vec3 filmic = x * (2.51 * x + 0.03) / (x * (2.43 * x + 0.59) + 0.14);
    gl_FragColor = vec4 (mix (c.rgb, filmic, strength / 100.0), c.a);
}
