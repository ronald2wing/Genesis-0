// SPDX-License-Identifier: GPL-3.0-or-later
// SPDX-FileCopyrightText: 2026 Genesis-0 contributors
//
// The fragment BODY; see genesis.exposure/effect.glsl for the contract.
// `width` and `height` are set by glshader every frame, not by the pack.
// `size` is an int parameter, laid out as a float uniform by the resolver.

uniform float width;
uniform float height;

void main () {
    float block = max (floor (size), 1.0);
    vec2 corner = floor (v_texcoord * vec2 (width, height) / block) * block;
    vec4 sum = vec4 (0.0);
    for (int y = 0; y < 4; y++) {
        for (int x = 0; x < 4; x++) {
            vec2 at = corner + (vec2 (float (x), float (y)) + vec2 (0.5)) * block / 4.0;
            sum += texture2D (tex, at / vec2 (width, height));
        }
    }
    gl_FragColor = sum / 16.0;
}
