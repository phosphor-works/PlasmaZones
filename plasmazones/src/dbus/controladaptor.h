// SPDX-FileCopyrightText: 2026 fuddlesworth
// SPDX-License-Identifier: GPL-3.0-or-later

#pragma once

#include "plasmazones_dbus_export.h"

#include <QDBusAbstractAdaptor>
#include <QDBusMessage>
#include <QFutureWatcher>
#include <QPointer>
#include <QString>

namespace PhosphorZones {
class LayoutRegistry;
}
namespace PhosphorEngine {
class IPlacementEngine;
}
namespace PhosphorScreens {
class ScreenManager;
}

namespace PhosphorEngineRuntime {
class ControlService;
class ScreenModeRouter;
} // namespace PhosphorEngineRuntime

namespace PlasmaZones {

class CompositorBridgeAdaptor;

/**
 * @brief The D-Bus binding of ControlService: org.phosphor_works.Placement.Control
 *
 * Every method and signal here is one the interface XML names, with the
 * signature it pins. The placement verbs forward to ControlService, where
 * they are documented. Two members are this binding's own, because they are
 * about plasmazonesd as a process on the bus rather than about placement:
 * generateSupportReport, which answers with a delayed reply and folds in the
 * compositor bridge's registration, and quit.
 */
class PLASMAZONES_DBUS_EXPORT ControlAdaptor : public QDBusAbstractAdaptor
{
    Q_OBJECT
    Q_CLASSINFO("D-Bus Interface", "org.phosphor_works.Placement.Control")

public:
    /// @param service Not owned. A call that arrives after it is gone is answered empty.
    /// The other pointers are the support report's sources, borrowed until detach().
    /// @param parent The object the adaptor is exported through
    ControlAdaptor(ControlService* service, PhosphorZones::LayoutRegistry* layoutManager,
                   PhosphorEngine::IPlacementEngine* autotileEngine, PhosphorScreens::ScreenManager* screenManager,
                   CompositorBridgeAdaptor* compositorBridge, PhosphorEngine::IPlacementEngine* scrollEngine,
                   const ScreenModeRouter* modeRouter, QObject* parent);
    ~ControlAdaptor() override;

    /// Null the support report's borrowed sources. Called from Daemon::withdrawFromBus()
    /// before the runtime's stop() destroys the unique_ptr members that own them; a report
    /// requested after it fails with Shutdown.
    void detach();

public Q_SLOTS:
    void snapWindowToZone(const QString& windowId, int zoneNumber, const QString& screenId);
    void toggleAutotileForScreen(const QString& screenId);
    QString getFullState();
    /**
     * @brief Generate a redacted support report for bug reports
     * @param sinceMinutes Minutes of journal logs to include (0 = default 30, max 120)
     * @return Empty string — the actual report is delivered asynchronously via delayed D-Bus reply.
     *         On error (concurrent call, shutdown), a D-Bus error reply is sent instead.
     */
    QString generateSupportReport(int sinceMinutes, const QDBusMessage& message);

    /**
     * @brief Ask the daemon to shut down cleanly
     *
     * The exit path is the ordinary one: this quits the event loop, so main()
     * resumes after exec() and runs Daemon::stop() plus the window teardown on
     * the main thread. Nothing is torn down from inside the D-Bus dispatch.
     *
     * This is what `plasmazonesd --replace` calls on the incumbent instance
     * before claiming the bus name. It is deliberately on the Control facade
     * rather than the Daemon object itself: Control is the documented
     * third-party entry point, and a shutdown verb needs no daemon internals.
     *
     * Safe to call when a shutdown is already in flight — QCoreApplication::quit()
     * on an already-quitting loop is a no-op.
     */
    void quit();
    QString getShortcutsJson();

Q_SIGNALS:
    void shortcutsChanged();

private:
    QPointer<ControlService> m_service;
    PhosphorZones::LayoutRegistry* m_layoutManager;
    PhosphorEngine::IPlacementEngine* m_autotileEngine;
    PhosphorScreens::ScreenManager* m_screenManager;
    CompositorBridgeAdaptor* m_compositorBridge;
    PhosphorEngine::IPlacementEngine* m_scrollEngine;
    const ScreenModeRouter* m_modeRouter;
    QPointer<QFutureWatcher<QString>> m_reportWatcher;
};

} // namespace PlasmaZones
