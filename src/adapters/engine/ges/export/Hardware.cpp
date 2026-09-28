// SPDX-License-Identifier: GPL-3.0-or-later
// SPDX-FileCopyrightText: 2026 Genesis-0 contributors

#include "adapters/engine/ges/export/Hardware.h"

#include <cctype>
#include <filesystem>
#include <string>
#include <string_view>
#include <vector>

#include "adapters/engine/ges/export/Encoder.h"

#include <gst/gl/egl/gstgldisplay_egl.h>
#include <gst/gl/gl.h>
#include <gst/gl/gstglfuncs.h>
#include <gst/gst.h>

namespace genesis::adapters::engine::ges {

namespace {

std::string lowercase(std::string_view in)
{
    std::string out;
    out.reserve(in.size());
    for (const unsigned char c : in) {
        out.push_back(static_cast<char>(std::tolower(c)));
    }
    return out;
}

// The GL renderer string, read back on the context's own GL thread. The probe
// borrows a throwaway offscreen context (the same EGL pattern every GL test
// uses) so it needs no window and no app GL context.
struct RendererReadback
{
    std::string renderer;
};

void read_renderer_on_gl(GstGLContext *context, gpointer user)
{
    auto *out = static_cast<RendererReadback *>(user);
    const GstGLFuncs *gl = context->gl_vtable;
    const GLubyte *name = gl->GetString(GL_RENDERER);
    out->renderer = name != nullptr ? reinterpret_cast<const char *>(name) : "";
}

std::string read_gl_renderer()
{
    GstGLDisplay *display = reinterpret_cast<GstGLDisplay *>(gst_gl_display_egl_new());
    if (display == nullptr) {
        return { };
    }
    GstGLContext *context = gst_gl_context_new(display);
    if (context == nullptr) {
        gst_object_unref(display);
        return { };
    }
    GError *error = nullptr;
    if (!gst_gl_context_create(context, nullptr, &error)) {
        g_clear_error(&error);
        gst_object_unref(context);
        gst_object_unref(display);
        return { };
    }
    RendererReadback readback;
    gst_gl_context_thread_add(context, read_renderer_on_gl, &readback);
    gst_object_unref(context);
    gst_object_unref(display);
    return readback.renderer;
}

// Whether a decoder factory is hardware-accelerated. Mirrors the classification
// the decode policy acts on (DecodePolicy.cpp): the engine marks hardware
// decoders with a "/Hardware" suffix in their klass, and that is the single
// source of truth for "can this machine decode in hardware".
bool is_hardware_decoder(GstElementFactory *factory)
{
    const gchar *klass = gst_element_factory_get_metadata(factory, GST_ELEMENT_METADATA_KLASS);
    return klass != nullptr && g_str_has_suffix(klass, "Hardware");
}

// The first hardware candidate whose element is present and reaches READY, or
// nullptr. The probe walks the `hardware` subset of the shared kH265Encoders
// list in the exporter's preference order, so the indicator agrees with what
// export would actually pick. Driving the element to READY is where a hardware
// encoder opens its device and a present-but-broken factory fails.
const char *first_usable_encoder()
{
    for (const EncoderCandidate &candidate : kH265Encoders) {
        if (candidate.hardware && element_usable(candidate.element)) {
            return candidate.element;
        }
    }
    return nullptr;
}

// The NVIDIA element factories whose presence signals an installed NVIDIA
// driver/stack. Covers the nvdec (nvh264dec/nvh265dec), nvenc (nvh264enc/
// nvh265enc), Jetson (nvv4l2*) and nvcuda (cuda*) families, plus the JPEG
// codecs. Presence is a factory lookup - the READY probe is what the encode
// selection does, and it is deliberately not re-run here.
constexpr const char *kNvidiaElements[] = {
    "nvh264dec",     "nvh265dec",   "nvh264enc",  "nvh265enc",    "nvv4l2decoder", "nvv4l2h264enc",
    "nvv4l2h265enc", "cudaconvert", "cudaupload", "cudadownload", "nvjpegdec",     "nvjpegenc",
};

} // namespace

std::vector<std::string> nvidia_elements_present()
{
    std::vector<std::string> found;
    for (const char *name : kNvidiaElements) {
        GstElementFactory *factory = gst_element_factory_find(name);
        if (factory != nullptr) {
            found.emplace_back(name);
            gst_object_unref(factory);
        }
    }
    return found;
}

bool nvidia_device_nodes_present()
{
    std::error_code ec;
    std::filesystem::directory_iterator it("/dev", ec);
    if (ec) {
        return false;
    }
    const std::filesystem::directory_iterator end;
    for (; it != end; ++it) {
        if (it->path().filename().string().rfind("nvidia", 0) == 0) {
            return true;
        }
    }
    return false;
}

bool is_software_renderer(std::string_view renderer)
{
    const std::string lowered = lowercase(renderer);
    return lowered.find("llvmpipe") != std::string::npos
            || lowered.find("softpipe") != std::string::npos
            || lowered.find("swrast") != std::string::npos
            || lowered.find("software") != std::string::npos;
}

bool element_usable(const char *name)
{
    GstElement *element = gst_element_factory_make(name, nullptr);
    if (element == nullptr) {
        return false;
    }
    const GstStateChangeReturn ret = gst_element_set_state(element, GST_STATE_READY);
    gst_element_set_state(element, GST_STATE_NULL);
    gst_object_unref(element);
    return ret != GST_STATE_CHANGE_FAILURE;
}

bool hardware_decode_available()
{
    GList *decoders =
            gst_element_factory_list_get_elements(GST_ELEMENT_FACTORY_TYPE_DECODER, GST_RANK_NONE);
    if (decoders == nullptr) {
        return false;
    }
    bool any = false;
    for (GList *it = decoders; it != nullptr; it = it->next) {
        auto *factory = GST_ELEMENT_FACTORY(it->data);
        if (is_hardware_decoder(factory)
            && gst_plugin_feature_get_rank(GST_PLUGIN_FEATURE(factory)) != GST_RANK_NONE) {
            any = true;
            break;
        }
    }
    gst_plugin_feature_list_free(decoders);
    return any;
}

HardwareInfo probe_hardware()
{
    HardwareInfo info;
    info.renderer = read_gl_renderer();
    info.software_gl = is_software_renderer(info.renderer);
    info.hardware_decode = hardware_decode_available();
    info.nvidia_elements = nvidia_elements_present();
    info.nvidia_available = !info.nvidia_elements.empty() || nvidia_device_nodes_present();
    const char *encoder = first_usable_encoder();
    if (encoder != nullptr) {
        info.hardware_encode = true;
        info.encode_element = encoder;
    }
    return info;
}

} // namespace genesis::adapters::engine::ges
