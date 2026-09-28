// SPDX-License-Identifier: GPL-3.0-or-later
// SPDX-FileCopyrightText: 2026 Genesis-0 contributors
//
// The fragment BODY; see genesis.exposure/effect.glsl for the contract.

vec3 kelvin (float k) {
    float t = clamp (k, 1000.0, 40000.0) / 100.0;
    float r, g, b;
    if (t <= 66.0) {
        r = 1.0;
        g = clamp ((99.4708 * log (t) - 161.1196) / 255.0, 0.0, 1.0);
        if (t <= 19.0) {
            b = 0.0;
        } else {
            b = clamp ((138.5177 * log (t - 10.0) - 305.0448) / 255.0, 0.0, 1.0);
        }
    } else {
        r = clamp (329.6987 * pow (t - 60.0, -0.1332) / 255.0, 0.0, 1.0);
        g = clamp (288.1222 * pow (t - 60.0, -0.0755) / 255.0, 0.0, 1.0);
        b = 1.0;
    }
    return vec3 (r, g, b);
}

void main () {
    vec4 c = texture2D (tex, v_texcoord);
    vec3 tint = kelvin (temperature) / kelvin (6500.0);
    float l = dot (tint, vec3 (0.2126, 0.7152, 0.0722));
    gl_FragColor = vec4 (c.rgb * (tint / max (l, 0.001)), c.a);
}
