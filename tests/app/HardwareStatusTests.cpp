// SPDX-License-Identifier: GPL-3.0-or-later
// SPDX-FileCopyrightText: 2026 Genesis-0 contributors

// The hardware-status controller's suite: drives the probe-derived properties
// and the decode-preference interaction headlessly, proving the property values
// and the changed() signal. No GStreamer and no GL: the probe result is
// injected as plain values through setProbe.

#include <cstdio>
#include <string>

#include <QCoreApplication>
#include <QObject>
#include <QStringList>

#include "app/controllers/HardwareStatus.h"
#include "../support/Checks.h"

namespace {

using genesis::test::check;

void test_defaults()
{
    HardwareStatus status;
    check(status.renderer().isEmpty(), "renderer defaults empty");
    check(!status.softwareGl(), "softwareGl defaults false");
    check(status.decode() == "software", "decode defaults to software");
    check(status.encode() == "software", "encode defaults to software");
    check(status.encodeElement().isEmpty(), "encodeElement defaults empty");
    check(!status.nvidiaAvailable(), "nvidiaAvailable defaults false");
    check(status.nvidiaElements().isEmpty(), "nvidiaElements defaults empty");
    check(status.nvidiaHint().isEmpty(), "nvidiaHint defaults empty");
    check(status.ai() == "CPU", "ai defaults to CPU");
}

void test_hardware_probe_case()
{
    HardwareStatus status;
    status.setProbe("NVIDIA GeForce RTX 3060", /*softwareGl=*/false,
                    /*hardwareDecode=*/true, /*hardwareEncode=*/true, "nvh265enc",
                    /*nvidiaAvailable=*/true, { "nvh265enc" });
    check(status.renderer() == "NVIDIA GeForce RTX 3060", "renderer set");
    check(!status.softwareGl(), "hardware renderer is not software");
    // Auto (0) + hardware decode available -> hardware.
    check(status.decode() == "hardware", "auto + available decodes in hardware");
    check(status.encode() == "hardware", "hardware encoder reported");
    check(status.encodeElement() == "nvh265enc", "the encoder element is named");
    check(status.nvidiaAvailable(), "NVIDIA is reported present");
    check(status.nvidiaElements() == QStringList{ "nvh265enc" },
          "the NVIDIA elements are reported");
    check(status.nvidiaHint().isEmpty(), "no offload hint when the renderer already is NVIDIA");
}

void test_nvidia_status_fields()
{
    // Hybrid/PRIME: the GL context landed on the Intel iGPU while NVIDIA is
    // present - the case the hint exists for.
    HardwareStatus hybrid;
    hybrid.setProbe("Mesa Intel(R) UHD Graphics 630", /*softwareGl=*/false,
                    /*hardwareDecode=*/true, /*hardwareEncode=*/true, "nvh265enc",
                    /*nvidiaAvailable=*/true, { "nvh265enc", "nvh264dec" });
    check(hybrid.nvidiaAvailable(), "hybrid reports NVIDIA present");
    check(hybrid.nvidiaElements() == QStringList{ "nvh265enc", "nvh264dec" },
          "hybrid reports the NVIDIA elements");
    const QString hint = hybrid.nvidiaHint();
    check(hint.contains("__NV_PRIME_RENDER_OFFLOAD=1"), "hint names the offload variable");
    check(hint.contains("__GLX_VENDOR_LIBRARY_NAME=nvidia"), "hint names the NVIDIA GLX variable");
    check(hint.contains("Mesa Intel(R) UHD Graphics 630"), "hint names the renderer");

    // No NVIDIA present: no hint regardless of the renderer.
    HardwareStatus absent;
    absent.setProbe("Mesa Intel(R) UHD Graphics 630", /*softwareGl=*/false,
                    /*hardwareDecode=*/false, /*hardwareEncode=*/false, "",
                    /*nvidiaAvailable=*/false, { });
    check(!absent.nvidiaAvailable(), "absent reports NVIDIA absent");
    check(absent.nvidiaHint().isEmpty(), "no hint when NVIDIA is absent");
}

void test_decode_preference_overrides()
{
    HardwareStatus status;
    status.setProbe("NVIDIA GeForce RTX 3060", /*softwareGl=*/false,
                    /*hardwareDecode=*/true, /*hardwareEncode=*/false, "",
                    /*nvidiaAvailable=*/true, { });

    // Software (1) forces the CPU path even when hardware exists.
    status.setDecodePreference(1);
    check(status.decode() == "software", "software preference forces software");

    // Hardware (2) + available -> hardware.
    status.setDecodePreference(2);
    check(status.decode() == "hardware", "hardware preference uses hardware");

    // Hardware preference + no hardware decode -> the engine falls back.
    HardwareStatus none;
    none.setProbe("llvmpipe (LLVM 16.0.6, 256 bits)", /*softwareGl=*/true,
                  /*hardwareDecode=*/false, /*hardwareEncode=*/false, "",
                  /*nvidiaAvailable=*/false, { });
    none.setDecodePreference(2);
    check(none.decode() == "software",
          "hardware preference without hardware falls back to software");
    check(none.softwareGl(), "the software renderer flags softwareGl");
}

void test_set_ai()
{
    HardwareStatus status;
    check(status.ai() == "CPU", "ai defaults to CPU");

    int notified = 0;
    QObject::connect(&status, &HardwareStatus::changed, [&] { ++notified; });

    status.setAi("CUDA");
    check(status.ai() == "CUDA", "setAi installs the mode");
    check(notified == 1, "a changed setAi emits once");

    // A no-op write emits nothing.
    status.setAi("CUDA");
    check(notified == 1, "a no-op setAi does not re-emit");

    status.setAi("CUDA (fallback CPU)");
    check(status.ai() == "CUDA (fallback CPU)", "setAi installs the fallback mode");
    check(notified == 2, "a changed setAi emits again");
}

void test_changed_signal()
{
    HardwareStatus status;
    int notified = 0;
    QObject::connect(&status, &HardwareStatus::changed, [&] { ++notified; });

    status.setProbe("NVIDIA GeForce RTX 3060", false, true, true, "nvh265enc",
                    /*nvidiaAvailable=*/true, { "nvh265enc" });
    check(notified >= 1, "setProbe emits changed");

    const int before = notified;
    status.setDecodePreference(1);
    check(notified == before + 1, "setDecodePreference emits changed");

    // A no-op preference write emits nothing.
    const int after = notified;
    status.setDecodePreference(1);
    check(notified == after, "a no-op preference write does not re-emit");
}

} // namespace

int main(int argc, char **argv)
{
    QCoreApplication app(argc, argv);

    test_defaults();
    test_hardware_probe_case();
    test_nvidia_status_fields();
    test_decode_preference_overrides();
    test_set_ai();
    test_changed_signal();

    return genesis::test::summary();
}
