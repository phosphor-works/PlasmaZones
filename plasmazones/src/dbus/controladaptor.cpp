// SPDX-FileCopyrightText: 2026 fuddlesworth
// SPDX-License-Identifier: GPL-3.0-or-later

#include "controladaptor.h"
#include "common/engineruntimenamespace.h"

#include "compositorbridgeadaptor.h"
#include <PhosphorEngineRuntime/Logging.h>
#include "core/platform/supportreport.h"
#include <PhosphorEngineRuntime/ControlService.h>

#include <PhosphorProtocol/ServiceConstants.h>

#include <QCoreApplication>
#include <QDBusConnection>
#include <QtConcurrent>

#include <memory>

namespace PlasmaZones {

ControlAdaptor::ControlAdaptor(ControlService* service, PhosphorZones::LayoutRegistry* layoutManager,
                               PhosphorEngine::IPlacementEngine* autotileEngine,
                               PhosphorScreens::ScreenManager* screenManager, CompositorBridgeAdaptor* compositorBridge,
                               PhosphorEngine::IPlacementEngine* scrollEngine, const ScreenModeRouter* modeRouter,
                               QObject* parent)
    : QDBusAbstractAdaptor(parent)
    , m_service(service)
    , m_layoutManager(layoutManager)
    , m_autotileEngine(autotileEngine)
    , m_screenManager(screenManager)
    , m_compositorBridge(compositorBridge)
    , m_scrollEngine(scrollEngine)
    , m_modeRouter(modeRouter)
{
    Q_ASSERT(service);
    connect(service, &ControlService::shortcutsChanged, this, &ControlAdaptor::shortcutsChanged);
}

ControlAdaptor::~ControlAdaptor() = default;

void ControlAdaptor::detach()
{
    m_layoutManager = nullptr;
    m_autotileEngine = nullptr;
    m_screenManager = nullptr;
    m_compositorBridge = nullptr;
    m_scrollEngine = nullptr;
    m_modeRouter = nullptr;
}

void ControlAdaptor::snapWindowToZone(const QString& windowId, int zoneNumber, const QString& screenId)
{
    if (m_service) {
        m_service->snapWindowToZone(windowId, zoneNumber, screenId);
    }
}

void ControlAdaptor::toggleAutotileForScreen(const QString& screenId)
{
    if (m_service) {
        m_service->toggleAutotileForScreen(screenId);
    }
}

QString ControlAdaptor::getFullState()
{
    return m_service ? m_service->getFullState() : QString();
}

QString ControlAdaptor::getShortcutsJson()
{
    return m_service ? m_service->getShortcutsJson() : QStringLiteral("[]");
}

QString ControlAdaptor::generateSupportReport(int sinceMinutes, const QDBusMessage& message)
{
    qCInfo(lcDbus) << "generateSupportReport: sinceMinutes=" << sinceMinutes;

    // Reject concurrent calls with a proper D-Bus error so callers get a typed failure
    // instead of having to parse the report string for "ERROR:".
    if (m_reportWatcher) {
        message.setDelayedReply(true);
        auto error = message.createErrorReply(QString(PhosphorProtocol::Service::Error::Busy),
                                              QStringLiteral("A support report is already being generated"));
        QDBusConnection::sessionBus().send(error);
        return {};
    }

    // Detach was called (shutdown in progress) — fail the report cleanly
    // instead of feeding null pointers into collectSnapshot.
    if (!m_screenManager || !m_layoutManager || !m_autotileEngine) {
        message.setDelayedReply(true);
        auto error = message.createErrorReply(QString(PhosphorProtocol::Service::Error::Shutdown),
                                              QStringLiteral("Daemon shutting down"));
        QDBusConnection::sessionBus().send(error);
        return {};
    }

    // Delay the D-Bus reply so we don't block the event loop while journalctl runs.
    message.setDelayedReply(true);

    // Snapshot QObject state on the main thread (these pointers are not thread-safe).
    auto snapshot = SupportReport::collectSnapshot(m_screenManager, m_layoutManager, m_autotileEngine, m_scrollEngine,
                                                   m_modeRouter);

    // Compositor bridge state lives in the dbus layer, so SupportReport (core/)
    // cannot read it directly — fold it into the snapshot here. Whether the
    // KWin effect has registered is the single most useful diagnostic for the
    // "dragging and shortcuts do nothing" class of bug.
    if (m_compositorBridge) {
        snapshot.hasBridgeInfo = true;
        snapshot.bridgeRegistered = m_compositorBridge->isBridgeRegistered();
        snapshot.bridgeName = m_compositorBridge->bridgeName();
        snapshot.bridgeVersion = m_compositorBridge->bridgeVersion();
        snapshot.bridgeCapabilities = m_compositorBridge->bridgeCapabilities();
    }

    // Run blocking work (file I/O, journalctl) off the main thread. No parent:
    // the watcher has to outlive the adaptor while the thread runs, and then
    // deletes itself.
    auto* watcher = new QFutureWatcher<QString>();
    m_reportWatcher = watcher;
    // Set once the caller has its answer, so an adaptor destroyed between the
    // reply and the watcher's deferred delete sends no second, Shutdown,
    // reply (F181).
    auto replied = std::make_shared<bool>(false);
    // The context is `this`, so Qt drops this connection when the adaptor
    // goes and the handler never runs against a dead adaptor.
    connect(watcher, &QFutureWatcher<QString>::finished, this, [this, message, watcher, replied]() {
        QDBusConnection::sessionBus().send(message.createReply(watcher->result()));
        *replied = true;
        m_reportWatcher = nullptr;
        watcher->deleteLater();
    });
    // The adaptor destroyed while the thread runs: answer Shutdown so the
    // caller does not wait out its timeout. The context is the watcher, so a
    // deleted watcher takes this connection with it. The disconnect stops a
    // finished signal still queued from replying after the error.
    connect(this, &QObject::destroyed, watcher, [message, watcher, replied]() {
        if (*replied) {
            return;
        }
        QObject::disconnect(watcher, &QFutureWatcher<QString>::finished, nullptr, nullptr);
        auto error = message.createErrorReply(QString(PhosphorProtocol::Service::Error::Shutdown),
                                              QStringLiteral("Daemon shutting down"));
        QDBusConnection::sessionBus().send(error);
        watcher->deleteLater();
    });
    watcher->setFuture(QtConcurrent::run([snapshot = std::move(snapshot), sinceMinutes]() {
        return SupportReport::generateFromSnapshot(snapshot, sinceMinutes);
    }));

    return {}; // Ignored — reply sent asynchronously
}

void ControlAdaptor::quit()
{
    qCInfo(lcDbus) << "Shutdown requested over D-Bus (org.phosphor_works.Placement.Control.quit)";
    // Return to main(), which owns the teardown order: Daemon::stop() then the
    // explicit window destroy loop. Doing either from here would run it inside
    // a D-Bus dispatch, with this adaptor's own parent among the objects torn
    // down under it.
    QCoreApplication::quit();
}

} // namespace PlasmaZones
