// SPDX-License-Identifier: GPL-3.0-or-later
// SPDX-FileCopyrightText: 2026 Genesis-0 contributors
//
// The fragment BODY; see genesis.exposure/effect.glsl for the contract.
// `width` and `height` are set by glshader every frame, not by the pack.

uniform float width;
uniform float height;

void main () {
    vec2 t = vec2 (1.0 / width, 1.0 / height);
    vec3 c = texture2D (tex, v_texcoord).rgb;
    float l = dot (c, vec3 (0.2126, 0.7152, 0.0722));
    float e = abs (dot (texture2D (tex, v_texcoord + vec2 (t.x, t.y)).rgb, vec3 (0.2126, 0.7152, 0.0722)) - l)
            + abs (dot (texture2D (tex, v_texcoord + vec2 (t.x, -t.y)).rgb, vec3 (0.2126, 0.7152, 0.0722)) - l);
    float dark = smoothstep (0.02, 0.08 + (1.0 - strength / 100.0) * 0.2, e);
    vec2 g = v_texcoord * 90.0;
    float hatch = step (0.5, fract (g.x + g.y)) * step (0.5, fract (g.x - g.y));
    gl_FragColor = vec4 (vec3 (l * mix (1.0, hatch, dark)), 1.0);
}
