// SPDX-License-Identifier: GPL-3.0-or-later
// SPDX-FileCopyrightText: 2026 Genesis-0 contributors

#pragma once

#include <QObject>
#include <QString>
#include <QStringList>

// The hardware-status controller: the read-only projection of the engine's
// hardware probe plus the decode preference, exposed to QML as the `gpu`
// context property. It owns no engine state - the probe result is installed
// through setProbe as plain values, so the controller stays engine-neutral and
// testable without GStreamer or a GL context.
class HardwareStatus : public QObject
{
    Q_OBJECT

    // The GL renderer string the probe read back, e.g. "NVIDIA GeForce RTX
    // 3060/PCIe/SSE2" or "llvmpipe (LLVM 16.0.6, 256 bits)"; empty when no GL
    // context could be made.
    Q_PROPERTY(QString renderer READ renderer NOTIFY changed)
    // Whether that renderer is a software rasteriser (llvmpipe/softpipe/swrast).
    Q_PROPERTY(bool softwareGl READ softwareGl NOTIFY changed)
    // The effective decode path: "hardware" when a hardware decoder exists and
    // the preference does not force software, "software" otherwise.
    Q_PROPERTY(QString decode READ decode NOTIFY changed)
    // The active decode policy: "auto", "software" or "hardware", the user's
    // preference the effective path above derives from.
    Q_PROPERTY(QString decodePolicy READ decodePolicy NOTIFY changed)
    // The effective encode path: "hardware" when a hardware encoder answered,
    // "software" otherwise.
    Q_PROPERTY(QString encode READ encode NOTIFY changed)
    // The encoder element that answered, or empty when encoding in software.
    Q_PROPERTY(QString encodeElement READ encodeElement NOTIFY changed)
    // Whether an NVIDIA GPU/driver stack is present, independent of the GL
    // renderer (element factories or /dev/nvidia*). True on a hybrid/PRIME
    // machine even when the GL context landed on the Intel iGPU.
    Q_PROPERTY(bool nvidiaAvailable READ nvidiaAvailable NOTIFY changed)
    // The NVIDIA element factories installed, e.g. ["nvh265enc", "nvh264dec"].
    Q_PROPERTY(QStringList nvidiaElements READ nvidiaElements NOTIFY changed)
    // The launch hint when the GL renderer is non-NVIDIA while NVIDIA is
    // present (the hybrid/PRIME case), or empty when not applicable.
    Q_PROPERTY(QString nvidiaHint READ nvidiaHint NOTIFY changed)
    // The AI execution mode. A canonical provider name - "CPU", "CUDA",
    // "TensorRT", "ROCm", "CoreML", "DirectML" or "OpenVINO" - with
    // " (fallback CPU)" appended when that provider's session had to retry on
    // CPU. Installed from the AI runtime through setAi.
    Q_PROPERTY(QString ai READ ai NOTIFY changed)

public:
    explicit HardwareStatus(QObject *parent = nullptr);

    QString renderer() const { return renderer_; }
    bool softwareGl() const { return softwareGl_; }
    QString decode() const;
    QString decodePolicy() const;
    QString encode() const
    {
        return hardwareEncode_ ? QStringLiteral("hardware") : QStringLiteral("software");
    }
    QString encodeElement() const { return encodeElement_; }
    bool nvidiaAvailable() const { return nvidiaAvailable_; }
    QStringList nvidiaElements() const { return nvidiaElements_; }
    QString nvidiaHint() const;
    QString ai() const { return ai_; }

    // Installs the engine probe's result as plain values.
    void setProbe(const QString &renderer, bool softwareGl, bool hardwareDecode,
                  bool hardwareEncode, const QString &encodeElement, bool nvidiaAvailable,
                  const QStringList &nvidiaElements);

    // Installs the AI execution mode reported by the AI runtime (a canonical
    // provider name, e.g. "CUDA" or "TensorRT", with " (fallback CPU)" appended
    // when that provider's session had to retry on CPU).
    void setAi(const QString &mode);

    // The playback decode preference (0 Auto, 1 Software, 2 Hardware), which
    // the decode path derives from.
    void setDecodePreference(int value);

signals:
    // Emitted whenever any exposed value changes.
    void changed();

private:
    QString renderer_;
    bool softwareGl_ = false;
    bool hardwareDecode_ = false;
    bool hardwareEncode_ = false;
    QString encodeElement_;
    bool nvidiaAvailable_ = false;
    QStringList nvidiaElements_;
    QString ai_ = QStringLiteral("CPU");
    int decodePreference_ = 0;
};
