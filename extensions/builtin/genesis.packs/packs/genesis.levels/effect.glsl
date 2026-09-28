// SPDX-License-Identifier: GPL-3.0-or-later
// SPDX-FileCopyrightText: 2026 Genesis-0 contributors
//
// The fragment BODY; see genesis.exposure/effect.glsl for the contract.

void main () {
    vec4 c = texture2D (tex, v_texcoord);
    vec3 rgb = clamp ((c.rgb - in_black) / max (in_white - in_black, 0.0001), 0.0, 1.0);
    rgb = pow (rgb, vec3 (1.0 / gamma));
    rgb = rgb * (out_white - out_black) + out_black;
    gl_FragColor = vec4 (rgb, c.a);
}
