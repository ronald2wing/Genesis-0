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
    vec3 sum = vec3 (0.0);
    for (int y = -1; y <= 1; y++) {
        for (int x = -1; x <= 1; x++) {
            sum += texture2D (tex, v_texcoord + vec2 (float (x), float (y)) * t * detail).rgb;
        }
    }
    vec3 blurred = sum / 9.0;
    vec3 deep = blurred * blurred * (3.0 - 2.0 * blurred);
    gl_FragColor = vec4 (mix (c.rgb, deep, strength / 100.0), c.a);
}
