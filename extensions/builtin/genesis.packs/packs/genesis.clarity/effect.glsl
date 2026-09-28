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
    float l = dot (c.rgb, vec3 (0.2126, 0.7152, 0.0722));
    float b = 0.0;
    for (int y = -2; y <= 2; y++) {
        for (int x = -2; x <= 2; x++) {
            b += dot (texture2D (tex, v_texcoord + vec2 (float (x), float (y)) * t * 2.0).rgb, vec3 (0.2126, 0.7152, 0.0722));
        }
    }
    b /= 25.0;
    float detail = l - b;
    gl_FragColor = vec4 (clamp (c.rgb + detail * strength / 100.0 * 2.0, 0.0, 1.0), c.a);
}
