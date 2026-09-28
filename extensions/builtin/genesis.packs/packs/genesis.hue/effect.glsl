// SPDX-License-Identifier: GPL-3.0-or-later
// SPDX-FileCopyrightText: 2026 Genesis-0 contributors
//
// A hue rotation in the YIQ plane (Concat's `hue_rotate`),
// SPDX-License-Identifier: GPL-3.0-or-later,
// SPDX-FileCopyrightText: 2026 Jareer and Concat contributors.
//
// The fragment BODY; see genesis.exposure/effect.glsl for the contract.

void main () {
    vec4 c = texture2D (tex, v_texcoord);
    vec3 rgb = c.rgb;
    float a = radians (degrees);
    float y = dot (rgb, vec3 (0.2126, 0.7152, 0.0722));
    float iq = dot (rgb, vec3 (0.596, -0.274, -0.322));
    float q = dot (rgb, vec3 (0.211, -0.523, 0.312));
    float i2 = iq * cos (a) - q * sin (a);
    float q2 = iq * sin (a) + q * cos (a);
    vec3 rotated = vec3 (
        y + 0.956 * i2 + 0.621 * q2,
        y - 0.272 * i2 - 0.647 * q2,
        y - 1.106 * i2 + 1.703 * q2);
    gl_FragColor = vec4 (rotated, c.a);
}
