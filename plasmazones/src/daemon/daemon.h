// SPDX-FileCopyrightText: 2026 fuddlesworth
// SPDX-License-Identifier: GPL-3.0-or-later

// plasmazonesd: the engine runtime as a KWin daemon. Daemon owns one
// EngineRuntime and everything that is about being a process on the session
// bus beside KWin and Plasma: the D-Bus bindings of the runtime's services,
// the bus name and object, the KWin plugin's protocol endpoints (the
// compositor bridge and the screen reports), the KWin and Plasma host
// implementations, the runtime's platform (wlr-layer-shell, Plasma's panels),
// logind's sleep notice, the bridge watchdog and the plasma-workspace gate.

#pragma once

#include "common/engineruntimenamespace.h"

#include <QDBusContext>
#include <QObject>
#include <QString>
#include <QTimer>

#include <memory>

namespace PhosphorScreens {
class DBusScreenAdaptor;
}

namespace PhosphorEngineRuntime {
class EngineRuntime;
class SettingsService;
} // namespace PhosphorEngineRuntime

namespace PlasmaZones {

class HostSettings;
class KWinWorkspaceHost;
class PlasmaSystemOsd;
class AutotileAdaptor;
class CompositorBridgeAdaptor;
class ControlAdaptor;
class LayoutAdaptor;
class OverlayAdaptor;
class RuleAdaptor;
class ScrollingAdaptor;
class SettingsAdaptor;
class ShaderAdaptor;
class SnapAdaptor;
class TilingAdaptor;
class WindowDragAdaptor;
class WindowTrackingAdaptor;
class ZoneDetectionAdaptor;

/**
 * @brief plasmazonesd: an EngineRuntime bound to the session bus beside KWin.
 *
 * A QDBusContext because Qt sets a bus call's context on the exported object,
 * not on the adaptor it reaches (DbusHelpers::busCaller), and every binding
 * below is a child of this object for that reason.
 */
class Daemon : public QObject, public QDBusContext
{
    Q_OBJECT

public:
    explicit Daemon(QObject* parent = nullptr);
    ~Daemon() override;

    /// Build the runtime's services and bind them to the bus.
    bool init();
    void start();
    void stop();

    EngineRuntime* runtime() const
    {
        return m_runtime.get();
    }

private:
    /// Bind the runtime's freshly built services. Drops the previous cycle's
    /// bindings first: the runtime deletes the services they forward to on
    /// every init().
    /// PlasmaZones' own settings (HostSettings) that the settings registry
    /// serves over the bus. The service is rebuilt on every init(), so this
    /// runs with each createBindings().
    void registerHostSettings(SettingsService* service);
    void createBindings();
    void deleteBindings();
    bool registerDBusService();
    /// The runtime's stop() reached the point where nothing may reach it from
    /// the bus any more: the shortcut grabs are gone, no service is detached yet.
    void withdrawFromBus();

    // ─── plasma-workspace gate (plasmaworkspace.cpp) ─────────────────────────
    /// Async query of systemd's user bus for `plasma-workspace.target` state.
    /// Fail-open on all D-Bus errors. Called once from `start()`.
    void queryPlasmaWorkspaceState();
    /// Continuation of `queryPlasmaWorkspaceState()`: fetches ActiveState and
    /// subscribes to PropertiesChanged after GetUnit resolves.
    void fetchPlasmaWorkspaceActiveState();
    /// Record the target's state and hand it to the runtime as whether the
    /// session is presentable.
    void setPlasmaWorkspaceActive(bool active);
    void releasePlasmaWorkspaceSubscription();

private Q_SLOTS:
    /// logind's PrepareForSleep, passed to the runtime.
    void onPrepareForSleep(bool goingToSleep);
    void onPlasmaWorkspaceTargetPropertiesChanged(const QString& interfaceName, const QVariantMap& changedProperties,
                                                  const QStringList& invalidatedProperties);

private:
    // ─── bridge watchdog (bridgewatchdog.cpp) ────────────────────────────────
    // If the KWin effect has not registered as a compositor bridge within a
    // grace period after startup, window control is dead (drags and shortcuts
    // do nothing). On timeout the daemon logs a diagnostic warning and raises
    // a desktop notification. Stopped early once the bridge registers.
    // Single-shot.
    void warnCompositorBridgeMissing();
    // Log a "compositor bridge missing" warning and raise a desktop
    // notification. `diagnosis` carries a specific root cause (e.g. an effect
    // plugin built against a different KWin version) when one was identified;
    // an empty string falls back to the generic enable-the-effect guidance.
    void emitBridgeMissingWarning(const QString& diagnosis);

    // Declared first so it is destroyed last: everything below borrows it.
    std::unique_ptr<EngineRuntime> m_runtime;

    // PlasmaZones' own settings, stored through the runtime's Settings.
    std::unique_ptr<HostSettings> m_hostSettings;

    // The host implementations the runtime reads through IWorkspaceHost and
    // ISystemOsd. Destroyed before the runtime, which stop() has already
    // released them from by then.
    std::unique_ptr<KWinWorkspaceHost> m_workspaceHost;
    std::unique_ptr<PlasmaSystemOsd> m_systemOsd;

    // The bus bindings of the runtime's services. QDBusAbstractAdaptor children
    // of this object (raw pointers). Each forwards to a service the runtime
    // owns, through a QPointer, and is deleted before the next init() rebuilds
    // those services.
    LayoutAdaptor* m_layoutAdaptor = nullptr;
    SettingsAdaptor* m_settingsAdaptor = nullptr;
    ShaderAdaptor* m_shaderAdaptor = nullptr;
    RuleAdaptor* m_ruleAdaptor = nullptr;
    OverlayAdaptor* m_overlayAdaptor = nullptr;
    ZoneDetectionAdaptor* m_zoneDetectionAdaptor = nullptr;
    WindowTrackingAdaptor* m_windowTrackingAdaptor = nullptr;
    WindowDragAdaptor* m_windowDragAdaptor = nullptr;
    SnapAdaptor* m_snapAdaptor = nullptr;
    TilingAdaptor* m_tilingAdaptor = nullptr;
    AutotileAdaptor* m_autotileAdaptor = nullptr;
    ScrollingAdaptor* m_scrollingAdaptor = nullptr;
    // Also carries the support report and quit; stop() detaches its borrows of
    // the runtime's engines before the runtime resets them.
    ControlAdaptor* m_controlAdaptor = nullptr;

    // The KWin plugin's protocol endpoints. The compositor bridge holds only
    // plain state; the screen adaptor writes the virtual-screen configuration
    // through the runtime's store.
    CompositorBridgeAdaptor* m_compositorBridge = nullptr;
    PhosphorScreens::DBusScreenAdaptor* m_screenAdaptor = nullptr;

    QTimer m_bridgeWatchdogTimer;
    // Grace period (ms) for the KWin effect to register as a compositor bridge
    // after daemon startup. Comfortably longer than a healthy effect takes to
    // register (sub-second once KWin and the daemon's D-Bus name are both up),
    // even when the daemon starts before KWin during login, so a timeout means
    // a genuine failure, not a race.
    static constexpr int BridgeWatchdogTimeoutMs = 20000;

    /// Mirrors `plasma-workspace.target` ActiveState on the user-bus systemd.
    /// `true` = real session, `false` = phantom/inactive. Defaults to `true`
    /// (fail-open for non-systemd / headless). See `queryPlasmaWorkspaceState()`
    /// for the full rationale.
    bool m_plasmaWorkspaceActive = true;
    /// D-Bus object path for `plasma-workspace.target`, resolved by GetUnit.
    QString m_plasmaWorkspaceTargetPath;

    /// Remembered so ~Daemon can disconnect the logind PrepareForSleep
    /// subscription explicitly rather than relying on QDBusConnection's
    /// internal receiver-destroyed detection (which works, but leaves a dead
    /// entry in the connection's slot table for the rest of the session).
    bool m_prepareForSleepConnected = false;

    /// Set by stop() and at aboutToQuit; the watchdog and the systemd replies
    /// stay silent once it is.
    bool m_shuttingDown = false;
    bool m_aboutToQuitConnected = false;
};

} // namespace PlasmaZones
