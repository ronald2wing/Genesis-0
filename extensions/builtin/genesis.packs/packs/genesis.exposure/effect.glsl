// SPDX-License-Identifier: GPL-3.0-or-later
// SPDX-FileCopyrightText: 2026 Genesis-0 contributors
//
// The fragment BODY. The engine's glshader prepends its prelude (precision,
// the varying, and the one texture `tex` the engine binds) and this pack's
// uniform declarations, then appends this file. It must not declare a second
// texture (the prelude owns the one bound) nor redeclare a parameter uniform.
// `width` and `height` are set by glshader every frame, not by the pack.

void main () {
    vec4 c = texture2D (tex, v_texcoord);
    gl_FragColor = vec4 (c.rgb * exp2 (stops), c.a);
}
