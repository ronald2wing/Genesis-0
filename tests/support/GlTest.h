// SPDX-License-Identifier: GPL-3.0-or-later
// SPDX-FileCopyrightText: 2026 Genesis-0 contributors
//
// Shared GL-test helper: a bounded retry around headless EGL display creation.
// The GL suites each create their own llvmpipe EGL context, and on a CI runner
// (Xvfb + llvmpipe, no /dev/dri) gst_gl_display_egl_new() intermittently
// returns null on the first call - a transient resource-acquisition failure,
// not a code defect. A short bounded retry absorbs it; the suites still fail
// loudly if EGL is genuinely unavailable. Engine-only (no Qt), so the
// engine-free adapter suites can include it.

#pragma once

#include <chrono>
#include <thread>

#include <gst/gl/egl/gstgldisplay_egl.h>
#include <gst/gl/gl.h>

namespace genesis::test {

// Creates a headless EGL display, retrying up to `attempts` times with a short
// pause between tries. Returns null only when every attempt failed.
inline GstGLDisplay *make_egl_display(int attempts = 5)
{
    for (int i = 0; i < attempts; ++i) {
        auto *display = reinterpret_cast<GstGLDisplay *>(gst_gl_display_egl_new());
        if (display != nullptr) {
            return display;
        }
        std::this_thread::sleep_for(std::chrono::milliseconds(200));
    }
    return nullptr;
}

} // namespace genesis::test
