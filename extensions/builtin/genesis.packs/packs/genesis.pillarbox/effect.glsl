// SPDX-License-Identifier: GPL-3.0-or-later
// SPDX-FileCopyrightText: 2026 Genesis-0 contributors
//
// The fragment BODY; see genesis.exposure/effect.glsl for the contract.
// `size` is an int parameter, laid out as a float uniform by the resolver.
// Each bar is `size` percent of the frame's width.

void main () {
    vec4 c = texture2D (tex, v_texcoord);
    float bar = size / 100.0;
    float in_bar = step (v_texcoord.x, bar) + step (1.0 - bar, v_texcoord.x);
    gl_FragColor = vec4 (mix (c.rgb, vec3 (0.0), clamp (in_bar, 0.0, 1.0)), c.a);
}
