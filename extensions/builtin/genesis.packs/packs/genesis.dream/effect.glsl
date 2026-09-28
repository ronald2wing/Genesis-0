// SPDX-License-Identifier: GPL-3.0-or-later
// SPDX-FileCopyrightText: 2026 Genesis-0 contributors
//
// The fragment BODY; see genesis.exposure/effect.glsl for the contract.

uniform float width;
uniform float height;

void main () {
    vec4 c = texture2D (tex, v_texcoord);
    vec2 px = vec2 (1.0 / width, 1.0 / height) * 6.0;
    vec3 soft = vec3 (0.0);
    soft += texture2D (tex, clamp (v_texcoord + vec2 (-px.x, 0.0), 0.0, 1.0)).rgb;
    soft += texture2D (tex, clamp (v_texcoord + vec2 (px.x, 0.0), 0.0, 1.0)).rgb;
    soft += texture2D (tex, clamp (v_texcoord + vec2 (0.0, -px.y), 0.0, 1.0)).rgb;
    soft += texture2D (tex, clamp (v_texcoord + vec2 (0.0, px.y), 0.0, 1.0)).rgb;
    soft = soft * 0.25;
    vec3 dreamy = soft + vec3 (0.06, 0.04, 0.03);
    gl_FragColor = vec4 (mix (c.rgb, dreamy, strength / 100.0), c.a);
}
