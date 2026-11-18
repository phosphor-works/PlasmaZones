// SPDX-FileCopyrightText: 2026 fuddlesworth
// SPDX-License-Identifier: GPL-3.0-or-later

// plasmazonesd's side of the engine runtime: the bus name and object, the
// bindings of the runtime's services, the KWin plugin's protocol endpoints and
// the host implementations. The bridge watchdog is in bridgewatchdog.cpp and
// the plasma-workspace.target probe in plasmaworkspace.cpp.

#include "daemon/daemon.h"

#include "common/hostsettings.h"
#include "config/settings.h"
#include "runtime/engine/engineruntime.h"
#include "runtime/services/settingsservice/settingsservice.h"
#include "daemon/host/kwinworkspacehost.h"
#include "daemon/host/plasmasystemosd.h"
#include "runtime/overlay/overlayservice.h"

#include "dbus/autotileadaptor.h"
#include "dbus/compositorbridgeadaptor.h"
#include "dbus/controladaptor.h"
#include "dbus/dbushelpers.h"
#include "dbus/layoutadaptor.h"
#include "dbus/overlayadaptor.h"
#include "dbus/ruleadaptor.h"
#include "dbus/scrollingadaptor.h"
#include "dbus/settingsadaptor.h"
#include "dbus/shaderadaptor.h"
#include "dbus/snapadaptor.h"
#include "dbus/tilingadaptor.h"
#include "dbus/windowdragadaptor.h"
#include "dbus/windowtrackingadaptor.h"
#include "dbus/zonedetectionadaptor.h"

#include "config/settingsconfigstore.h"
#include "core/platform/logging.h"

#include <PhosphorLayer/defaults/PhosphorWaylandTransport.h>
#include <PhosphorProtocol/ServiceConstants.h>
#include <PhosphorScreens/DBusScreenAdaptor.h>
#include <PhosphorScreens/PlasmaPanelSource.h>
#include <PhosphorShortcuts/Factory.h>
#include <PhosphorShortcuts/IBackend.h>
#include <PhosphorWorkspaces/Plasma/Backends.h>

#include <QDBusConnection>
#include <QDBusError>
#include <QGuiApplication>

namespace PlasmaZones {

namespace {
const QString Login1Service = QStringLiteral("org.freedesktop.login1");
const QString Login1Path = QStringLiteral("/org/freedesktop/login1");
const QString Login1ManagerInterface = QStringLiteral("org.freedesktop.login1.Manager");
const QString PrepareForSleepSignal = QStringLiteral("PrepareForSleep");

/// plasmazonesd's overlays are wlr-layer-shell surfaces, and its panels,
/// virtual desktops and activities are Plasma's and KWin's.
EngineRuntime::Platform kwinSessionPlatform()
{
    EngineRuntime::Platform platform;
    platform.layerShell = std::make_unique<PhosphorLayer::PhosphorWaylandTransport>();
    platform.panels = std::make_unique<PhosphorScreens::PlasmaPanelSource>();
    platform.desktops = PhosphorWorkspaces::Plasma::createVirtualDesktopBackend();
    platform.activities = PhosphorWorkspaces::Plasma::createActivityBackend();
    // KGlobalAccel on Plasma, the portal or the session-bus fallback
    // elsewhere: PhosphorShortcuts reads the session.
    platform.shortcutBackends = [] {
        return PhosphorShortcuts::createBackend(PhosphorShortcuts::BackendHint::Auto, nullptr);
    };
    return platform;
}
} // namespace

Daemon::Daemon(QObject* parent)
    : QObject(parent)
    , m_runtime(std::make_unique<EngineRuntime>(kwinSessionPlatform()))
    , m_hostSettings(std::make_unique<HostSettings>(m_runtime->settings()))
    , m_workspaceHost(std::make_unique<KWinWorkspaceHost>(m_runtime->shortcutManager(), m_runtime->settings(),
                                                          m_hostSettings.get()))
    , m_systemOsd(std::make_unique<PlasmaSystemOsd>())
{
    m_runtime->setWorkspaceHost(m_workspaceHost.get());
    // The KWin host's two preferences are PlasmaZones' own settings: the
    // runtime hears of a change through the host's report.
    for (const auto changed :
         {&HostSettings::workspacesManageKWinPerOutputChanged, &HostSettings::workspacesRebindKWinShortcutsChanged}) {
        connect(m_hostSettings.get(), changed, m_runtime.get(), &EngineRuntime::workspaceHostPreferencesChanged);
    }
    m_runtime->setSystemOsd(m_systemOsd.get());

    // Direct: the runtime's stop() emits this mid-teardown and relies on the
    // withdrawal having happened by the time the emit returns.
    connect(m_runtime.get(), &EngineRuntime::servicesClosing, this, &Daemon::withdrawFromBus, Qt::DirectConnection);

    m_bridgeWatchdogTimer.setSingleShot(true);
    connect(&m_bridgeWatchdogTimer, &QTimer::timeout, this, &Daemon::warnCompositorBridgeMissing);

    // System sleep/resume via logind, so the runtime can restart its shader
    // clock after a wake instead of handing the first frame the whole suspend
    // as its delta. The connect result is kept so ~Daemon can disconnect.
    m_prepareForSleepConnected = QDBusConnection::systemBus().connect(
        Login1Service, Login1Path, Login1ManagerInterface, PrepareForSleepSignal, this, SLOT(onPrepareForSleep(bool)));
    if (!m_prepareForSleepConnected) {
        qCDebug(lcDaemon) << "PrepareForSleep D-Bus signal subscription failed (logind not available?):"
                          << "shader-timer restart on resume will not run";
    }
}

Daemon::~Daemon()
{
    if (m_prepareForSleepConnected) {
        QDBusConnection::systemBus().disconnect(Login1Service, Login1Path, Login1ManagerInterface,
                                                PrepareForSleepSignal, this, SLOT(onPrepareForSleep(bool)));
        m_prepareForSleepConnected = false;
    }
    stop();
    // The bindings are Qt children, which ~QObject destroys only after every
    // member, the runtime included, is gone. Delete them while what they
    // borrow is still alive.
    deleteBindings();
    // The runtime stops in its own destructor too; release its borrows of the
    // hosts first, which are destroyed before it.
    m_runtime->setWorkspaceHost(nullptr);
    m_runtime->setSystemOsd(nullptr);
}

bool Daemon::init()
{
    // The runtime deletes and rebuilds its services on every init(), so the
    // previous cycle's bindings go first: they forward to those services.
    deleteBindings();
    if (!m_runtime->init()) {
        return false;
    }
    createBindings();
    return registerDBusService();
}

void Daemon::start()
{
    m_shuttingDown = false;
    // Connected once, so a stop() -> start() cycle does not stack it.
    if (qGuiApp && !m_aboutToQuitConnected) {
        connect(qGuiApp, &QGuiApplication::aboutToQuit, this, [this]() {
            m_shuttingDown = true;
        });
        m_aboutToQuitConnected = true;
    }

    // Detect phantom plasma-restore sessions via systemd's user bus. See
    // queryPlasmaWorkspaceState() for the full rationale.
    queryPlasmaWorkspaceState();

    m_runtime->start();

    // Arm the compositor-bridge registration watchdog. See
    // BridgeWatchdogTimeoutMs for the grace-period rationale. Skip if the
    // effect already registered during init().
    if (m_compositorBridge && !m_compositorBridge->isBridgeRegistered()) {
        m_bridgeWatchdogTimer.start(BridgeWatchdogTimeoutMs);
    }
}

void Daemon::stop()
{
    m_shuttingDown = true;
    // A late watchdog fire is harmless (it returns on m_shuttingDown), but
    // every other piece of this teardown severs explicitly rather than
    // relying on an invariant.
    m_bridgeWatchdogTimer.stop();
    releasePlasmaWorkspaceSubscription();
    // The runtime releases the shortcut grabs and the dynamic workspaces, then
    // emits servicesClosing (withdrawFromBus) before it detaches anything.
    m_runtime->stop();
}

void Daemon::onPrepareForSleep(bool goingToSleep)
{
    m_runtime->prepareForSleep(goingToSleep);
}

void Daemon::withdrawFromBus()
{
    // Release the bus name only now. The runtime's stop() has released the
    // shortcut grabs and torn the dynamic workspaces down before emitting,
    // and both must reach kglobalacceld and KWin ahead of anything a
    // --replace or bus-activated successor sends once it owns the name (see
    // EngineRuntime::stop()). And no late call may land in the teardown the
    // runtime runs after this.
    QDBusConnection bus = QDBusConnection::sessionBus();
    bus.unregisterObject(QString(PhosphorProtocol::Service::ObjectPath));
    bus.unregisterService(QString(PhosphorProtocol::Service::Name));

    // The support report's sources are the runtime's engines and managers,
    // which its stop() resets next.
    if (m_controlAdaptor) {
        m_controlAdaptor->detach();
    }
    // A `this`-capturing hook into a binding the runtime outlives.
    if (m_windowTrackingAdaptor) {
        m_windowTrackingAdaptor->setBridgeCallerCheck({});
    }
}

void Daemon::deleteBindings()
{
    // Not deleteLater(): init() runs before exec(), so no queued D-Bus
    // dispatch can land mid-delete, and a deleteLater() would not be processed
    // before the new set registers over the same interfaces on the same
    // object, which Qt refuses for a second adaptor of one interface.
    // Borrowers before borrowed: the control binding borrows the bridge.
    delete m_controlAdaptor;
    m_controlAdaptor = nullptr;
    delete m_scrollingAdaptor;
    m_scrollingAdaptor = nullptr;
    delete m_autotileAdaptor;
    m_autotileAdaptor = nullptr;
    delete m_tilingAdaptor;
    m_tilingAdaptor = nullptr;
    delete m_snapAdaptor;
    m_snapAdaptor = nullptr;
    delete m_windowDragAdaptor;
    m_windowDragAdaptor = nullptr;
    delete m_screenAdaptor;
    m_screenAdaptor = nullptr;
    delete m_windowTrackingAdaptor;
    m_windowTrackingAdaptor = nullptr;
    delete m_zoneDetectionAdaptor;
    m_zoneDetectionAdaptor = nullptr;
    delete m_overlayAdaptor;
    m_overlayAdaptor = nullptr;
    delete m_compositorBridge;
    m_compositorBridge = nullptr;
    delete m_ruleAdaptor;
    m_ruleAdaptor = nullptr;
    delete m_shaderAdaptor;
    m_shaderAdaptor = nullptr;
    delete m_settingsAdaptor;
    m_settingsAdaptor = nullptr;
    delete m_layoutAdaptor;
    m_layoutAdaptor = nullptr;
}

void Daemon::registerHostSettings(SettingsService* service)
{
    HostSettings* const host = m_hostSettings.get();
    // A malformed token is refused at the boundary rather than coerced: the
    // schema's coercion would store the default and setSetting would report a
    // write that never happened. The bus carries only {key, type}, no choices.
    service->registerHostSetting(
        QStringLiteral("renderingBackend"), QStringLiteral("string"), HostSettings::renderingGroup(),
        HostSettings::backendKey(),
        [host]() {
            return QVariant(host->renderingBackend());
        },
        [host](const QVariant& value) {
            const QString requested = value.toString().toLower().trimmed();
            if (!HostSettings::renderingBackendOptions().contains(requested)) {
                return false;
            }
            host->setRenderingBackend(requested);
            return true;
        });
    // Only "auto" or a well-formed vendor:device pair, in any case or
    // whitespace variation that normalizes to itself.
    service->registerHostSetting(
        QStringLiteral("gpuDevice"), QStringLiteral("string"), HostSettings::renderingGroup(), HostSettings::gpuKey(),
        [host]() {
            return QVariant(host->gpuDevice());
        },
        [host](const QVariant& value) {
            const QString raw = value.toString();
            const QString normalized = HostSettings::normalizeGpuDevice(raw);
            if (normalized != raw.toLower().trimmed()) {
                return false;
            }
            host->setGpuDevice(normalized);
            return true;
        });
}

void Daemon::createBindings()
{
    EngineRuntime* const rt = m_runtime.get();

    m_layoutAdaptor = new LayoutAdaptor(rt->layoutService(), this);
    m_settingsAdaptor = new SettingsAdaptor(rt->settingsService(), this);
    registerHostSettings(rt->settingsService());
    m_shaderAdaptor = new ShaderAdaptor(rt->shaderService(), this);
    m_ruleAdaptor = new RuleAdaptor(rt->ruleService(), this);

    // Compositor bridge: the compositor-agnostic window control protocol.
    // Held as a member so the support report and the registration watchdog
    // can query its state.
    m_compositorBridge = new CompositorBridgeAdaptor(this);
    connect(m_compositorBridge, &CompositorBridgeAdaptor::bridgeRegistered, this,
            [this](const QString& compositorName, const QString&, const QStringList&) {
                m_bridgeWatchdogTimer.stop();
                m_runtime->compositorConnected(compositorName);
            });

    // Also carries the KWin effect's thumbnail ingest.
    //
    // overlayVisibilityChanged is OUTBOUND ONLY: the binding relays
    // OverlayService::visibilityChanged onto the bus for clients. Nothing here
    // subscribes to it. Something once did, routing the signal straight back
    // into showOverlay/hideOverlay, so every internal show or hide re-entered
    // the service that had just emitted it: an unguarded feedback cycle.
    // Inbound overlay control comes from the binding's own D-Bus methods.
    m_overlayAdaptor = new OverlayAdaptor(rt->overlayControlService(), rt->overlayService(), this);
    // A bridge registers only from the compositor's own process, the one the
    // thumbnail entries are held to, so no other peer can take the
    // registration first and keep KWin out (#1155). Both are children of
    // this daemon and go together.
    m_compositorBridge->setCompositorIdentityCheck([overlay = m_overlayAdaptor](const QString& busName) {
        return overlay->isCompositorPeer(busName);
    });

    m_zoneDetectionAdaptor = new ZoneDetectionAdaptor(rt->zoneQueryService(), this);

    m_windowTrackingAdaptor = new WindowTrackingAdaptor(rt->windowTracker(), this);
    // The compositor bridge's screen-form floats act on the window where it
    // is; any other bus caller's are gated as the screenless verb is (F1108).
    m_windowTrackingAdaptor->setBridgeCallerCheck([adaptor = m_windowTrackingAdaptor](const QString& busName) {
        return DbusHelpers::isBridgeCaller(adaptor, busName);
    });

    // DBusScreenAdaptor::setVirtualScreenConfig writes to Settings (the source
    // of truth) through the runtime's single SettingsConfigStore, shared with
    // its screen manager and virtual-screen swapper. One store per process,
    // one change-signal channel, no parallel Settings observer.
    m_screenAdaptor = new PhosphorScreens::DBusScreenAdaptor(rt->screenManager(), rt->virtualScreenStore(), this);
    // Its compositor reports answer to the registered bridge through the same
    // gate as the daemon's own compositor-only slots (#1155).
    m_screenAdaptor->setCompositorReportGate([adaptor = m_screenAdaptor](QLatin1StringView member) {
        return DbusHelpers::admitCompositorReport(adaptor, member);
    });

    m_windowDragAdaptor = new WindowDragAdaptor(rt->dragResolver(), this);
    m_snapAdaptor = new SnapAdaptor(rt->snapService(), this);
    m_tilingAdaptor = new TilingAdaptor(rt->tilingService(), this);
    m_autotileAdaptor = new AutotileAdaptor(rt->autotileService(), this);
    m_scrollingAdaptor = new ScrollingAdaptor(rt->scrollingService(), this);

    m_controlAdaptor =
        new ControlAdaptor(rt->controlService(), rt->layoutManager(), rt->autotileEngine(), rt->screenManager(),
                           m_compositorBridge, rt->scrollEngine(), rt->screenModeRouter(), this);
}

bool Daemon::registerDBusService()
{
    auto bus = QDBusConnection::sessionBus();
    if (!bus.isConnected()) {
        qCCritical(lcDaemon) << "Session D-Bus: cannot connect, daemon cannot function";
        return false;
    }

    // Claim the well-known name. In this process the claim has ALREADY been made:
    // main() registers the name as its single-instance gate, before constructing
    // the Daemon, because that is the only place a duplicate start can exit 0
    // cheaply (exiting non-zero there would put systemd's Restart=on-failure into
    // a loop on every duplicate autostart). registerService on a name the same
    // connection already owns returns true, so this call is the confirmation step,
    // not a second claim.
    //
    // It is kept rather than assumed because init() must not register its object
    // against a name this process does not hold, and because withdrawFromBus()
    // releases the name, so any future re-init needs the claim to happen here.
    //
    // There is deliberately no retry loop. An earlier version backed off over
    // 300 ms and classified ServiceUnknown/NoReply as transient, but none of that
    // is reachable: main() has already completed a successful round trip to the
    // bus, so by the time init() runs the bus is up and the name is ours. A false
    // here means the ordering above changed, which is a programming error and
    // wants a clear message, not a sleep.
    if (!bus.registerService(QString(PhosphorProtocol::Service::Name))) {
        const QDBusError error = bus.lastError();
        if (error.type() == QDBusError::NoError) {
            qCCritical(lcDaemon) << "Failed to register D-Bus service=" << PhosphorProtocol::Service::Name
                                 << "— the name is owned by another connection. main() claims it before"
                                 << "constructing the Daemon, so reaching this means that gate was bypassed.";
        } else {
            qCCritical(lcDaemon) << "Failed to register D-Bus service=" << PhosphorProtocol::Service::Name
                                 << "error=" << error.message() << "type=" << error.type();
        }
        return false;
    }

    if (!bus.registerObject(QString(PhosphorProtocol::Service::ObjectPath), this)) {
        const QDBusError error = bus.lastError();
        qCCritical(lcDaemon) << "Failed to register D-Bus object=" << PhosphorProtocol::Service::ObjectPath
                             << "error=" << error.message();
        bus.unregisterService(QString(PhosphorProtocol::Service::Name));
        return false;
    }

    qCInfo(lcDaemon) << "D-Bus service registered service=" << PhosphorProtocol::Service::Name
                     << "path=" << PhosphorProtocol::Service::ObjectPath;
    return true;
}

} // namespace PlasmaZones
