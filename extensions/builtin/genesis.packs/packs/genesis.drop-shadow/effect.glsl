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
    vec2 off = vec2 (offset_x, offset_y) * t;
    float shadow = 0.0;
    for (int y = -2; y <= 2; y++) {
        for (int x = -2; x <= 2; x++) {
            shadow += texture2D (tex, v_texcoord - off + vec2 (float (x), float (y)) * t * blur).a;
        }
    }
    shadow /= 25.0;
    float a = shadow * (opacity / 100.0);
    vec3 result = mix (c.rgb, vec3 (0.0), a * (1.0 - c.a));
    gl_FragColor = vec4 (result, max (c.a, a));
}
