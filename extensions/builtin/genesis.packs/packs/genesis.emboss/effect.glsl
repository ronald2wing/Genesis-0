// SPDX-License-Identifier: GPL-3.0-or-later
// SPDX-FileCopyrightText: 2026 Genesis-0 contributors
//
// The fragment BODY; see genesis.exposure/effect.glsl for the contract.
// `width` and `height` are set by glshader every frame, not by the pack.

uniform float width;
uniform float height;

void main () {
    vec2 t = vec2 (1.0 / width, 1.0 / height);
    float l = dot (texture2D (tex, v_texcoord + vec2 (-t.x, -t.y)).rgb, vec3 (0.2126, 0.7152, 0.0722));
    float r = dot (texture2D (tex, v_texcoord + vec2 (t.x, t.y)).rgb, vec3 (0.2126, 0.7152, 0.0722));
    float e = (r - l) * (strength / 100.0) * 2.0 + 0.5;
    gl_FragColor = vec4 (vec3 (e), 1.0);
}
