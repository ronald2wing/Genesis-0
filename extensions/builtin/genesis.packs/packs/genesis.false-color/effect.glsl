// SPDX-License-Identifier: GPL-3.0-or-later
// SPDX-FileCopyrightText: 2026 Genesis-0 contributors
//
// The fragment BODY; see genesis.exposure/effect.glsl for the contract.

vec3 thermal (float l) {
    vec3 c = vec3 (0.0);
    c = mix (c, vec3 (1.0, 0.0, 0.0), smoothstep (0.0, 0.4, l));
    c = mix (c, vec3 (1.0, 1.0, 0.0), smoothstep (0.4, 0.7, l));
    c = mix (c, vec3 (1.0, 1.0, 1.0), smoothstep (0.7, 1.0, l));
    return c;
}

vec3 ice (float l) {
    vec3 c = vec3 (0.0);
    c = mix (c, vec3 (0.0, 0.0, 1.0), smoothstep (0.0, 0.4, l));
    c = mix (c, vec3 (0.0, 1.0, 1.0), smoothstep (0.4, 0.7, l));
    c = mix (c, vec3 (1.0, 1.0, 1.0), smoothstep (0.7, 1.0, l));
    return c;
}

vec3 rainbow (float l) {
    return 0.5 + 0.5 * cos (6.28318 * (l * 3.0 + vec3 (0.0, 0.33, 0.67)));
}

void main () {
    vec4 c = texture2D (tex, v_texcoord);
    float l = dot (c.rgb, vec3 (0.2126, 0.7152, 0.0722));
    vec3 col;
    if (palette < 0.5) {
        col = thermal (l);
    } else if (palette < 1.5) {
        col = ice (l);
    } else {
        col = rainbow (l);
    }
    gl_FragColor = vec4 (col, c.a);
}
