// SPDX-License-Identifier: GPL-3.0-or-later
// SPDX-FileCopyrightText: 2026 Genesis-0 contributors
//
// Re-authored from Concat's concat-effects package `concat.vhs`,
// SPDX-License-Identifier: GPL-3.0-or-later,
// SPDX-FileCopyrightText: 2026 Jareer and Concat contributors.
//
// The fragment BODY; see genesis.exposure/effect.glsl for the contract.
// `width` and `height` are set by glshader every frame, not by the pack.

uniform float width;
uniform float height;

void main () {
    vec2 uv = v_texcoord;
    float s = strength / 100.0;
    vec2 mid = uv - vec2 (0.5);
    vec2 curved = uv + mid * dot (mid, mid) * s * 0.15;
    float ca = s * 3.0 / width;
    float red = texture2D (tex, curved + vec2 (ca, 0.0)).r;
    float green = texture2D (tex, curved).g;
    float blue = texture2D (tex, curved - vec2 (ca, 0.0)).b;
    vec3 rgb = vec3 (red, green, blue);
    float row = fract (v_texcoord.y * height / 3.0);
    rgb *= 1.0 - s * 0.5 * step (0.5, row);
    rgb *= 1.0 - s * 0.5 * smoothstep (0.4, 0.9, length (mid));
    gl_FragColor = vec4 (rgb, 1.0);
}
