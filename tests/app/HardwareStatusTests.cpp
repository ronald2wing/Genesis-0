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

#include "app/controllers/HardwareStatus.h"

namespace {

int checks = 0;
int failures = 0;

void check(bool condition, const std::string &what)
{
    ++checks;
    if (!condition) {
        ++failures;
        std::printf("FAIL: %s\n", what.c_str());
    }
}

void test_defaults()
{
    HardwareStatus status;
    check(status.renderer().isEmpty(), "renderer defaults empty");
    check(!status.softwareGl(), "softwareGl defaults false");
    check(status.decode() == "software", "decode defaults to software");
    check(status.encode() == "software", "encode defaults to software");
    check(status.encodeElement().isEmpty(), "encodeElement defaults empty");
    check(status.ai() == "CPU", "ai defaults to CPU");
}

void test_hardware_probe_case()
{
    HardwareStatus status;
    status.setProbe("NVIDIA GeForce RTX 3060", /*softwareGl=*/false,
                    /*hardwareDecode=*/true, /*hardwareEncode=*/true, "nvh265enc");
    check(status.renderer() == "NVIDIA GeForce RTX 3060", "renderer set");
    check(!status.softwareGl(), "hardware renderer is not software");
    // Auto (0) + hardware decode available -> hardware.
    check(status.decode() == "hardware", "auto + available decodes in hardware");
    check(status.encode() == "hardware", "hardware encoder reported");
    check(status.encodeElement() == "nvh265enc", "the encoder element is named");
}

void test_decode_preference_overrides()
{
    HardwareStatus status;
    status.setProbe("NVIDIA GeForce RTX 3060", /*softwareGl=*/false,
                    /*hardwareDecode=*/true, /*hardwareEncode=*/false, "");

    // Software (1) forces the CPU path even when hardware exists.
    status.setDecodePreference(1);
    check(status.decode() == "software", "software preference forces software");

    // Hardware (2) + available -> hardware.
    status.setDecodePreference(2);
    check(status.decode() == "hardware", "hardware preference uses hardware");

    // Hardware preference + no hardware decode -> the engine falls back.
    HardwareStatus none;
    none.setProbe("llvmpipe (LLVM 16.0.6, 256 bits)", /*softwareGl=*/true,
                  /*hardwareDecode=*/false, /*hardwareEncode=*/false, "");
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

    status.setProbe("NVIDIA GeForce RTX 3060", false, true, true, "nvh265enc");
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
    test_decode_preference_overrides();
    test_set_ai();
    test_changed_signal();

    std::printf("%d checks, %d failures\n", checks, failures);
    return failures == 0 ? 0 : 1;
}
