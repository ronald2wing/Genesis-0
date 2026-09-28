// SPDX-License-Identifier: GPL-3.0-or-later
// SPDX-FileCopyrightText: 2026 Genesis-0 contributors
//
// The seam's client-API name -> the GStreamer GLAPI bit a wrapped context is
// created with. Unknown keeps the GLES2 default: right for the headless GLES
// context, and the caller only reaches here after its own API detection failed.

#pragma once

#include <gst/gl/gl.h>

#include "adapters/engine/EngineSession.h"

namespace genesis::adapters::engine::ges {

inline GstGLAPI gst_gl_api_from(engine::GlApi api)
{
    switch (api) {
    case engine::GlApi::OpenGL:
        return GST_GL_API_OPENGL;
    case engine::GlApi::OpenGL3:
        return GST_GL_API_OPENGL3;
    case engine::GlApi::GLES2:
    case engine::GlApi::Unknown:
        break;
    }
    return GST_GL_API_GLES2;
}

} // namespace genesis::adapters::engine::ges
