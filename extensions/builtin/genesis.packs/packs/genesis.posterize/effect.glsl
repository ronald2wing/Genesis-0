// SPDX-License-Identifier: GPL-3.0-or-later
// SPDX-FileCopyrightText: 2026 Genesis-0 contributors
//
// Re-authored from Concat's concat-effects package `concat.posterize`,
// SPDX-License-Identifier: GPL-3.0-or-later,
// SPDX-FileCopyrightText: 2026 Jareer and Concat contributors.
//
// The fragment BODY; see genesis.exposure/effect.glsl for the contract.
// `levels` is an int parameter, laid out as a float uniform by the resolver.

void main () {
    vec4 c = texture2D (tex, v_texcoord);
    float steps = max (floor (levels), 2.0) - 1.0;
    vec3 bands = floor (c.rgb * steps + 0.5) / steps;
    gl_FragColor = vec4 (bands, c.a);
}
