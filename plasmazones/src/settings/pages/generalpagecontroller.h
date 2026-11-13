// SPDX-FileCopyrightText: 2026 fuddlesworth
// SPDX-License-Identifier: GPL-3.0-or-later

#pragma once

#include "config/configdefaults.h"

#include <PhosphorControl/PageController.h>
#include <QObject>
#include <QString>
#include <QStringList>
#include <QVariant>

namespace PlasmaZones {

class ISettings;

/// Q_PROPERTY surface for the "General" settings page.
///
/// Owns the startup-time rendering-backend and GPU-device snapshots that keep
/// the "restart required" InlineMessage visible across page navigation (the
/// backend picker's options come from SettingsController::valueOptions and
/// the schema), the enumerated GPU list for the rendering-device picker, the
/// CONSTANT bounds of the Shader Effects and Audio Spectrum sliders with a
/// one-shot `cavaAvailable` probe, plus the animation-duration / min-distance
/// / stagger-interval slider bounds that the animation curve editors consume,
/// plus the snap-side and animation-side window-filtering bounds that the
/// shared WindowFilterCard reads. The decoration-side filtering bounds live on
/// WindowAppearanceController, the controller of the page that hosts them.
///
/// Import/export of the full config stays on SettingsController — those are
/// top-level app actions that touch every page, not a "General" concern.
///
/// No per-page staged state; isDirty/apply/discard are no-ops. Value-based
/// dirty tracking for General flows through the pageOwnedConfigKeys manifest
/// (settingscontroller_pagetopology.cpp), with SettingsController's
/// meta-object NOTIFY loop attributing edits to the active page.
class GeneralPageController : public PhosphorControl::PageController
{
    Q_OBJECT

    Q_PROPERTY(QString startupRenderingBackend READ startupRenderingBackend NOTIFY startupSnapshotsChanged)
    Q_PROPERTY(QString startupGpuDevice READ startupGpuDevice NOTIFY startupSnapshotsChanged)

    // {text, value} rows for the GPU picker: "auto" first, then the
    // machine's GPUs from DRM render-node enumeration (GpuDeviceList,
    // enumerated once per process — a GPU hot-plugged mid-session appears
    // after the settings app restarts). A stored value that no longer
    // matches any present GPU (card removed, config copied from another
    // machine) is appended as a synthetic "Unavailable device" row so the
    // combo can still display the persisted selection; the row is rebuilt on
    // every gpuDeviceChanged so external writes stay displayable and the
    // synthetic row disappears once a present device is selected.
    Q_PROPERTY(QVariantList availableGpus READ availableGpus NOTIFY availableGpusChanged)

    // Shader Effects + Audio Spectrum slider bounds. Frame rate and the
    // audio spectrum drive every shader category, so these cards live on the
    // General page. Live values bind directly via `appSettings.X`.
    Q_PROPERTY(int shaderFrameRateMin READ shaderFrameRateMin CONSTANT)
    Q_PROPERTY(int shaderFrameRateMax READ shaderFrameRateMax CONSTANT)
    Q_PROPERTY(int audioSpectrumBarCountMin READ audioSpectrumBarCountMin CONSTANT)
    Q_PROPERTY(int audioSpectrumBarCountMax READ audioSpectrumBarCountMax CONSTANT)
    Q_PROPERTY(int audioSensitivityMin READ audioSensitivityMin CONSTANT)
    Q_PROPERTY(int audioSensitivityMax READ audioSensitivityMax CONSTANT)
    Q_PROPERTY(int audioNoiseReductionMin READ audioNoiseReductionMin CONSTANT)
    Q_PROPERTY(int audioNoiseReductionMax READ audioNoiseReductionMax CONSTANT)
    Q_PROPERTY(int audioLowerCutoffHzMin READ audioLowerCutoffHzMin CONSTANT)
    Q_PROPERTY(int audioLowerCutoffHzMax READ audioLowerCutoffHzMax CONSTANT)
    Q_PROPERTY(int audioHigherCutoffHzMin READ audioHigherCutoffHzMin CONSTANT)
    Q_PROPERTY(int audioHigherCutoffHzMax READ audioHigherCutoffHzMax CONSTANT)
    Q_PROPERTY(int audioExtraSmoothingMin READ audioExtraSmoothingMin CONSTANT)
    Q_PROPERTY(int audioExtraSmoothingMax READ audioExtraSmoothingMax CONSTANT)
    // Whether the cava executable was on PATH when this controller was built.
    // Probed once: installing cava mid-session shows up after a restart.
    Q_PROPERTY(bool cavaAvailable READ cavaAvailable CONSTANT)

    Q_PROPERTY(int animationDurationMin READ animationDurationMin CONSTANT)
    Q_PROPERTY(int animationDurationMax READ animationDurationMax CONSTANT)
    Q_PROPERTY(int animationMinDistanceMin READ animationMinDistanceMin CONSTANT)
    Q_PROPERTY(int animationMinDistanceMax READ animationMinDistanceMax CONSTANT)
    Q_PROPERTY(int animationStaggerIntervalMin READ animationStaggerIntervalMin CONSTANT)
    Q_PROPERTY(int animationStaggerIntervalMax READ animationStaggerIntervalMax CONSTANT)

    // Window-filtering SpinBox bounds — bound from the schema, NOT from
    // QML literals, so the SpinBox visible range always tracks the
    // schema-allowed range. A user value persisted at the high end of
    // the schema range (e.g. 1500 px when the schema allows 0–2000)
    // would otherwise silently truncate when the SpinBox clamped the
    // bound `value` to the QML literal. Both the snap-side
    // (GeneralPage's "Window filtering" card) and the animation-side
    // (AnimationWindowFilterCard's filtering knobs) bind through these.
    Q_PROPERTY(int minimumWindowWidthMin READ minimumWindowWidthMin CONSTANT)
    Q_PROPERTY(int minimumWindowWidthMax READ minimumWindowWidthMax CONSTANT)
    Q_PROPERTY(int minimumWindowHeightMin READ minimumWindowHeightMin CONSTANT)
    Q_PROPERTY(int minimumWindowHeightMax READ minimumWindowHeightMax CONSTANT)
    Q_PROPERTY(int animationMinimumWindowWidthMin READ animationMinimumWindowWidthMin CONSTANT)
    Q_PROPERTY(int animationMinimumWindowWidthMax READ animationMinimumWindowWidthMax CONSTANT)
    Q_PROPERTY(int animationMinimumWindowHeightMin READ animationMinimumWindowHeightMin CONSTANT)
    Q_PROPERTY(int animationMinimumWindowHeightMax READ animationMinimumWindowHeightMax CONSTANT)

public:
    /// Reference parameter, not pointer: the ISettings instance is required
    /// at construction time (to snapshot the current rendering backend) and
    /// must not be null. Taking it by reference makes the precondition a
    /// compile-time guarantee. ISettings (not the concrete Settings) per
    /// CLAUDE.md so unit tests can stub.
    explicit GeneralPageController(ISettings& settings, QObject* parent = nullptr);

    bool isDirty() const override
    {
        return false;
    }
    void apply() override
    {
    }
    void discard() override
    {
    }

    /// Re-snapshot the startup backend/GPU values from the live settings.
    /// SettingsController calls this on the daemon's running rising edge:
    /// a freshly started daemon has just read the current config, so the
    /// "restart required" banner must stop comparing against the values the
    /// SETTINGS APP started with.
    void rebaselineStartupSnapshots();

    QString startupRenderingBackend() const
    {
        return m_startupRenderingBackend;
    }
    QString startupGpuDevice() const
    {
        return m_startupGpuDevice;
    }
    QVariantList availableGpus() const
    {
        return m_availableGpus;
    }

    int shaderFrameRateMin() const
    {
        return ConfigDefaults::shaderFrameRateMin();
    }
    int shaderFrameRateMax() const
    {
        return ConfigDefaults::shaderFrameRateMax();
    }
    int audioSpectrumBarCountMin() const
    {
        return ConfigDefaults::audioSpectrumBarCountMin();
    }
    int audioSpectrumBarCountMax() const
    {
        return ConfigDefaults::audioSpectrumBarCountMax();
    }
    int audioSensitivityMin() const
    {
        return ConfigDefaults::audioSensitivityMin();
    }
    int audioSensitivityMax() const
    {
        return ConfigDefaults::audioSensitivityMax();
    }
    int audioNoiseReductionMin() const
    {
        return ConfigDefaults::audioNoiseReductionMin();
    }
    int audioNoiseReductionMax() const
    {
        return ConfigDefaults::audioNoiseReductionMax();
    }
    int audioLowerCutoffHzMin() const
    {
        return ConfigDefaults::audioLowerCutoffHzMin();
    }
    int audioLowerCutoffHzMax() const
    {
        return ConfigDefaults::audioLowerCutoffHzMax();
    }
    int audioHigherCutoffHzMin() const
    {
        return ConfigDefaults::audioHigherCutoffHzMin();
    }
    int audioHigherCutoffHzMax() const
    {
        return ConfigDefaults::audioHigherCutoffHzMax();
    }
    int audioExtraSmoothingMin() const
    {
        return ConfigDefaults::audioExtraSmoothingMin();
    }
    int audioExtraSmoothingMax() const
    {
        return ConfigDefaults::audioExtraSmoothingMax();
    }
    bool cavaAvailable() const
    {
        return m_cavaAvailable;
    }

    int animationDurationMin() const
    {
        return ConfigDefaults::animationDurationMin();
    }
    int animationDurationMax() const
    {
        return ConfigDefaults::animationDurationMax();
    }
    int animationMinDistanceMin() const
    {
        return ConfigDefaults::animationMinDistanceMin();
    }
    int animationMinDistanceMax() const
    {
        return ConfigDefaults::animationMinDistanceMax();
    }
    int animationStaggerIntervalMin() const
    {
        return ConfigDefaults::animationStaggerIntervalMin();
    }
    int animationStaggerIntervalMax() const
    {
        return ConfigDefaults::animationStaggerIntervalMax();
    }

    int minimumWindowWidthMin() const
    {
        return ConfigDefaults::minimumWindowWidthMin();
    }
    int minimumWindowWidthMax() const
    {
        return ConfigDefaults::minimumWindowWidthMax();
    }
    int minimumWindowHeightMin() const
    {
        return ConfigDefaults::minimumWindowHeightMin();
    }
    int minimumWindowHeightMax() const
    {
        return ConfigDefaults::minimumWindowHeightMax();
    }
    int animationMinimumWindowWidthMin() const
    {
        return ConfigDefaults::animationMinimumWindowWidthMin();
    }
    int animationMinimumWindowWidthMax() const
    {
        return ConfigDefaults::animationMinimumWindowWidthMax();
    }
    int animationMinimumWindowHeightMin() const
    {
        return ConfigDefaults::animationMinimumWindowHeightMin();
    }
    int animationMinimumWindowHeightMax() const
    {
        return ConfigDefaults::animationMinimumWindowHeightMax();
    }

Q_SIGNALS:
    /// Both startup snapshots were re-baselined (daemon restart observed).
    void startupSnapshotsChanged();
    /// The GPU picker rows changed (external gpuDevice write added or
    /// removed the synthetic "Unavailable device" row).
    void availableGpusChanged();

private Q_SLOTS:
    /// Rebuild the picker rows from the cached enumeration + the current
    /// stored value. Connected to ISettings::gpuDeviceChanged.
    void rebuildAvailableGpus();

private:
    ISettings& m_settings;

    /// Backend / GPU values at controller construction (re-baselined on
    /// daemon restart). Survive page recreation so the "restart required"
    /// InlineMessage stays visible after navigating away and back.
    QString m_startupRenderingBackend;
    QString m_startupGpuDevice;

    /// Raw GpuDeviceList::enumerate() result, cached once per process.
    QVariantList m_enumeratedGpus;
    QVariantList m_availableGpus;

    /// cava executable lookup, done once at construction.
    bool m_cavaAvailable = false;
};

} // namespace PlasmaZones
