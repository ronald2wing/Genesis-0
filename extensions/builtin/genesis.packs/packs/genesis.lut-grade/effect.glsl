// SPDX-License-Identifier: GPL-3.0-or-later
// SPDX-FileCopyrightText: 2026 Genesis-0 contributors
//
// The fragment BODY; see genesis.exposure/effect.glsl for the contract.
//
// `lut` is an enum over the eight shipped .cube tables (assets/luts/). The
// host's LUT path (the `lutgrade` GstGLFilter element) binds the real table:
// the resolver loads the sorted table at the enum's index (LutLoader) into a
// core::Lut, and lutgrade uploads it as a 3D texture its shader samples. The
// glshader path here binds one 2D texture - the picture - and cannot sample a
// 3D LUT, so when no table resolves, each selection carries a fixed
// approximation of the table's cast instead, mixed from full strength down to
// the untouched picture. This block is the engine-side fallback; the real
// table wins whenever the host resolves one and routes through lutgrade.

void main () {
    vec4 c = texture2D (tex, v_texcoord);
    float pick = floor (lut + 0.5);
    vec3 grade = c.rgb;
    if (pick == 0.0) { grade = vec3 (c.r * 1.02, c.g * 1.00, c.b * 1.08); }
    else if (pick == 1.0) { grade = vec3 (c.r * 1.06, c.g * 1.01, c.b * 0.90); }
    else if (pick == 2.0) { grade = vec3 (c.r * 1.04, c.g * 1.02, c.b * 0.92); }
    else if (pick == 3.0) { grade = vec3 (c.r * 0.98, c.g * 1.03, c.b * 1.06); }
    else if (pick == 4.0) { grade = vec3 (c.r * 1.03, c.g * 1.01, c.b * 1.00); }
    else { grade = vec3 (c.r * 1.05, c.g * 1.01, c.b * 1.03); }
    gl_FragColor = vec4 (mix (c.rgb, grade, strength / 100.0), c.a);
}
