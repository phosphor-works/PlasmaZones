// SPDX-FileCopyrightText: 2026 fuddlesworth
// SPDX-License-Identifier: GPL-3.0-or-later

#include "daemon.h"

#include "config/configdefaults.h"
#include "config/configmigration.h"
#include "core/types/constants.h"
#include "core/platform/logging.h"
#include "core/utils/translationloader.h"
#include "phosphor_i18n.h"
#include "rendering/zoneshaderitem.h"
#include "version.h"
#include "rendering/vulkansupport.h"
#include "replaceincumbent.h"
#include "waylandsessioncheck.h"

#include <PhosphorConfig/HostDirectory.h>
#include <PhosphorProtocol/Registration.h>
#include <PhosphorProtocol/ServiceConstants.h>
#include <PhosphorWayland/LayerShellPluginLoader.h>
#include <PhosphorWayland/LayerSurface.h>

#include <QByteArray>
#include <QCommandLineOption>
#include <QCommandLineParser>
#include <QCoreApplication>
#include <QDBusConnection>
#include <QDBusConnectionInterface>
#include <QDBusError>
#include <QDBusMessage>
#include <QDBusPendingCall>
#include <QDBusReply>
#include <QFile>
#include <QGuiApplication>
#include <QIcon>
#include <QList>
#include <QMutex>
#include <QMutexLocker>
#include <QSocketNotifier>
#include <QStringList>
#include <QThread>
#include <QVariantMap>
#include <QWindow>
#include <QtQml/qqml.h>
#include <QtQml/qqmlextensionplugin.h>

#include <cerrno>
#include <cstddef>
#include <cstdio>
#include <cstring>
#include <fcntl.h>
#include <iterator>
#include <limits>
#include <optional>
#include <signal.h>
#include <unistd.h>

// Import static QML module for shared components
Q_IMPORT_QML_PLUGIN(org_plasmazones_commonPlugin)

using namespace PlasmaZones;

namespace {

// The KGlobalAccel component key, and the name the parser prints for --version
// and in the --help usage line. Named once because two sites set it and they
// must not drift: the early help/version path at the top of main() and normal
// startup below. Must match plasmazonesd.desktop.
constexpr const char* kApplicationName = "plasmazonesd";

// The options this daemon accepts, handed back so callers can query them.
// Held by value because QCommandLineParser::isSet/value take the option object.
struct DaemonOptions
{
    QCommandLineOption replace;
    QCommandLineOption debug;
    QCommandLineOption logFile;
};

// THE option table — the only one. Called twice: once from the early
// help/version path at the top of main(), which runs before QGuiApplication
// exists, and once during normal startup. One function rather than two lists
// is the whole point: a flag added here appears in --help and is accepted by
// the daemon in the same edit, and --help cannot come to describe a set of
// options the daemon does not have.
DaemonOptions configureParser(QCommandLineParser& parser)
{
    parser.setApplicationDescription(PhosphorI18n::tr("Window snapping, tiling and scrolling"));
    parser.addHelpOption();
    parser.addVersionOption();

    DaemonOptions options{
        QCommandLineOption(QStringList{QStringLiteral("r"), QStringLiteral("replace")},
                           PhosphorI18n::tr("Replace existing daemon instance")),
        QCommandLineOption(QStringList{QStringLiteral("d"), QStringLiteral("debug")},
                           PhosphorI18n::tr("Enable debug logging for all PlasmaZones categories")),
        QCommandLineOption(QStringList{QStringLiteral("l"), QStringLiteral("log-file")},
                           PhosphorI18n::tr("Write log output to <file> instead of stderr"), PhosphorI18n::tr("file")),
    };
    parser.addOption(options.replace);
    parser.addOption(options.debug);
    parser.addOption(options.logFile);
    return options;
}

// The environment variables main() changes for THIS process only. Each is set
// or unset because of what the daemon is (a background service with several
// QQuickWindows on layer-shell surfaces), which says nothing about the editor
// or the settings app, so a child must not inherit the change. Every variable
// main() touches before QGuiApplication belongs in this table, except the
// GPU-preference ones, which rendering/vulkansupport.cpp tracks itself.
constexpr const char* kDaemonPrivateEnvVars[] = {
    "MANGOHUD",
    "DISABLE_MANGOHUD",
    "QSG_USE_SIMPLE_ANIMATION_DRIVER",
    "QT_WAYLAND_SHELL_INTEGRATION",
};

// One variable's state: disengaged when unset, else its value (which may be
// empty, a different thing from unset).
std::optional<QByteArray> environmentValue(const char* name)
{
    return qEnvironmentVariableIsSet(name) ? std::optional<QByteArray>(qgetenv(name)) : std::nullopt;
}

// kDaemonPrivateEnvVars as this process inherited them, taken before main()
// changes any. In table order.
QList<std::optional<QByteArray>> inheritedPrivateEnvironment()
{
    QList<std::optional<QByteArray>> inherited;
    for (const char* name : kDaemonPrivateEnvVars) {
        inherited.append(environmentValue(name));
    }
    return inherited;
}

// What main() did to kDaemonPrivateEnvVars, in the shape the spawn sites undo
// it (PDaemonEnvIntroducedVarsProperty / PDaemonEnvInheritedVarsProperty in
// core/types/constants.h, read by childProcessEnvironment() in
// core/platform/childprocessenvironment.h). A variable that already had the daemon's value in
// the inherited environment appears in neither: the user set it that way, and
// it reaches a child unchanged.
struct PrivateEnvironmentChanges
{
    // Unset when inherited and set now: a child has it removed.
    QStringList introduced;
    // Set when inherited and unset or different now, with the inherited
    // value: a child has it put back.
    QVariantMap inherited;
};

PrivateEnvironmentChanges privateEnvironmentChanges(const QList<std::optional<QByteArray>>& inherited)
{
    PrivateEnvironmentChanges changes;
    qsizetype index = 0;
    for (const char* name : kDaemonPrivateEnvVars) {
        const std::optional<QByteArray>& before = inherited.at(index++);
        if (before == environmentValue(name)) {
            continue;
        }
        if (before) {
            changes.inherited.insert(QString::fromLatin1(name), QString::fromLocal8Bit(*before));
        } else {
            changes.introduced.append(QString::fromLatin1(name));
        }
    }
    return changes;
}

// Self-pipe for signal delivery. The handler may only touch async-signal-safe
// calls, so it writes one byte here and returns; the real shutdown runs on the
// main thread from a QSocketNotifier slot.
//
// This is not a style preference. Daemon::stop() acquires QMutexes, frees heap,
// makes blocking D-Bus round trips (unregisterObject / unregisterService), tears
// down QFileSystemWatchers and joins a QThreadPool (waitForDone(500) in stop(),
// then the pool's destructor joins whatever is still running). Running
// that from signal context is a real deadlock: under --log-file the installed
// message handler takes logMutex, so a signal arriving while any thread already
// holds it wedges the process on the first log line stop() emits.
int g_signalPipe[2] = {-1, -1};

extern "C" void signalHandler(int signum)
{
    // The handler can interrupt code that is between a failing call and its
    // read of errno, and write() below may set it, so put back what was there.
    const int savedErrno = errno;
    const unsigned char byte = static_cast<unsigned char>(signum);
    // write() is async-signal-safe. A full pipe means a wakeup is already
    // queued, so a dropped byte changes nothing; the result is deliberately
    // ignored rather than logged (logging here is what we are avoiding).
    const ssize_t written = ::write(g_signalPipe[1], &byte, 1);
    static_cast<void>(written);
    errno = savedErrno;
}

// The dispositions SIGINT, SIGTERM and SIGHUP had before signalHandler
// replaced them, and whether it did. A daemon started under nohup inherits
// SIGHUP ignored, and keeps it ignored once the handler is gone.
constexpr int kHandledSignals[] = {SIGINT, SIGTERM, SIGHUP};
sighandler_t g_inheritedDispositions[std::size(kHandledSignals)] = {SIG_DFL, SIG_DFL, SIG_DFL};
bool g_signalHandlerInstalled = false;

void installSignalHandler()
{
    for (std::size_t i = 0; i < std::size(kHandledSignals); ++i) {
        g_inheritedDispositions[i] = signal(kHandledSignals[i], signalHandler);
    }
    g_signalHandlerInstalled = true;
}

// Give SIGINT, SIGTERM and SIGHUP back the dispositions the process started
// with. Called on every path that leaves the event loop behind: with no loop
// reading g_signalPipe a signal would be written and never acted on, so from
// here a signal has to act on the process itself. Does nothing when the
// handler was never installed.
void restoreInheritedSignalDispositions()
{
    if (!g_signalHandlerInstalled) {
        return;
    }
    for (std::size_t i = 0; i < std::size(kHandledSignals); ++i) {
        signal(kHandledSignals[i], g_inheritedDispositions[i]);
    }
    g_signalHandlerInstalled = false;
}

// How long --replace waits, once the bus name is ours, for the incumbent
// PROCESS to exit, and how often it looks. The budget comes from what the
// incumbent still has to do after Daemon::stop() releases the name. Some of
// those steps bound their own wait: PersistenceWorker's destructor and
// PhosphorZones::LayoutComputeService's destructor each wait for a thread,
// stop() waits for the shader-bake pool, and CavaSpectrumProvider::stop()
// waits for its process. stop() runs twice, from main() and again from
// Daemon's destructor, and the pool wait comes before its once-only part, so
// that wait counts twice. The budget is those bounds added up and rounded up,
// so it covers the case where each of them runs out. The steps with no bound
// of their own (the synchronous session, layout and settings saves, the
// shortcut and bus round trips, the bake pool's destructor joining what is
// still running, and the window and application teardown) are not budgeted:
// an incumbent that takes longer is warned about and not waited for, so a
// hung one costs --replace a bounded delay.
constexpr int kIncumbentExitBudgetMs = 12500;
constexpr int kIncumbentExitPollMs = 50;

// Wait up to kIncumbentExitBudgetMs for process @p pid to exit. True when
// there is nothing left to wait for, false when it was still there at the end
// of the budget. The caller passes a pid above zero that is not this process.
//
// kill() with signal 0 delivers nothing and only reports whether the pid can
// be signalled. Any failure ends the wait: ESRCH is the process gone, and
// EPERM is a pid this user may not signal, which cannot be the daemon that
// held this session's bus name (a reused pid, or one from another pid
// namespace). A process that has exited but that its parent has not reaped
// still answers as alive, so an unreaped incumbent costs the full budget.
bool waitForProcessExit(pid_t pid)
{
    for (int waitedMs = 0;; waitedMs += kIncumbentExitPollMs) {
        if (::kill(pid, 0) != 0) {
            return true;
        }
        if (waitedMs >= kIncumbentExitBudgetMs) {
            return false;
        }
        QThread::msleep(kIncumbentExitPollMs);
    }
}

// The incumbent's pid for --replace from the placement host record, when the
// bus could not name it. believedIncumbentPid() (replaceincumbent.h) decides
// whether the record still describes a live plasmazonesd. 0 when it does not.
pid_t recordedIncumbentPid()
{
    return PlasmaZones::believedIncumbentPid(
        PhosphorConfig::HostDirectory(QString(PhosphorProtocol::Service::HostApi)).record(),
        QString(PlasmaZones::PlacementHostId), QCoreApplication::applicationPid());
}

// The exit status for a session this daemon does not serve: no usable
// Wayland display, or no KWin on the session bus. EX_UNAVAILABLE from
// sysexits.h. plasmazones.service lists it in RestartPreventExitStatus=, so
// systemd does not restart the daemon for it, and records the start as
// failed. Not 0, because a start that ends inactive is indistinguishable
// from a successful one: systemd reports the start job as done and sends no
// activation failure, and dbus-broker's launcher fails a pending bus
// activation only when the unit's start job fails or the unit enters
// "failed". With exit 0, a client call that bus-activated the daemon in
// such a session waited until its own reply timeout while dbus-broker kept
// the activation pending. test_daemon_lifecycle_order ties this value to the
// unit file.
constexpr int kExitSessionNotServed = 69;

// What the session bus said about org.kde.KWin. Unknown is kept apart from
// Absent because the two need opposite exits: Absent is a session this daemon
// does not serve (kExitSessionNotServed, nothing for systemd to retry), Unknown is a bus that
// could not be asked (exit 1, the same hard failure as no bus at all).
enum class KWinPresence {
    Present,
    Absent,
    Unknown,
};

// org.phosphor_works.Placement has one host per session. plasmazonesd is the
// host under KWin, and the Phosphor compositor is the intended second host, for
// its own session. This is the KWin host, so it runs only where KWin is on the
// bus. The systemd unit is already scoped to plasma-workspace.target; this
// covers D-Bus activation, autostart and a manual start. At login KWin can take
// its name a moment after this process starts, so wait a bounded time before
// deciding.
KWinPresence waitForKWin(QDBusConnection& bus)
{
    QDBusConnectionInterface* const iface = bus.interface();
    if (!iface) {
        qCCritical(PlasmaZones::lcDaemon)
            << "The session bus connection has no bus interface, so org.kde.KWin cannot be looked up";
        return KWinPresence::Unknown;
    }
    const QString kwinService = QStringLiteral("org.kde.KWin");
    // The reply is kept, not read as a bool: a NameHasOwner call that FAILED
    // converts to false, which is indistinguishable from "no owner" once the
    // QDBusReply is gone.
    QDBusReply<bool> reply;
    // 25 x 200ms: five seconds.
    for (int attempt = 0; attempt < 25; ++attempt) {
        reply = iface->isServiceRegistered(kwinService);
        if (reply.isValid() && reply.value()) {
            return KWinPresence::Present;
        }
        QThread::msleep(200);
    }
    reply = iface->isServiceRegistered(kwinService);
    if (!reply.isValid()) {
        qCCritical(PlasmaZones::lcDaemon)
            << "Could not ask the session bus whether org.kde.KWin is running:" << reply.error().message();
        return KWinPresence::Unknown;
    }
    return reply.value() ? KWinPresence::Present : KWinPresence::Absent;
}

} // namespace

int main(int argc, char* argv[])
{
    // --help and --version are answered HERE, before everything else, because
    // everything below assumes a live session: the Wayland guard exits cleanly
    // on a machine with no compositor, and the real parser is not reached until
    // the bottom of this function, behind a QGuiApplication whose constructor
    // is the very thing that guard exists to avoid calling headless. Without
    // this, asking the binary what it is — from a package build, a container or
    // a bug report — printed nothing on stdout and exited 0.
    //
    // A QCoreApplication rather than a hand-rolled argv scan. It needs no
    // display, and it is what makes this authentic rather than a reimplementation:
    //   - configureParser() below is the SAME function normal startup calls, so
    //     --help lists exactly the flags the daemon accepts and cannot drift
    //     from them.
    //   - the parser knows which options take values, so `--log-file --version`
    //     is correctly read as a log file named "--version" and falls through to
    //     start the daemon, as it always did. A scan matching tokens anywhere in
    //     argv gets that wrong.
    //   - loadTranslations() runs here too, so headless --help is translated
    //     identically to in-session --help.
    // Without a QCoreApplication the usage line reads "<executable_name>"
    // instead of the program name, so the instance is required, not incidental.
    //
    // parse() rather than process(): process() treats an unknown option as a
    // fatal error, which would move ALL argument validation ahead of the
    // Wayland guard and change what a bad flag does on a headless box. parse()
    // only populates, leaving that behaviour where it was.
    //
    // Scoped so the QCoreApplication is destroyed before the QGuiApplication
    // below is constructed. showHelp()/showVersion() exit, so the fall-through
    // path is the one where neither was asked for.
    {
        QCoreApplication probe(argc, argv);
        QCoreApplication::setApplicationName(QString::fromLatin1(kApplicationName));
        QCoreApplication::setApplicationVersion(PlasmaZones::VERSION_STRING);
        PlasmaZones::loadTranslations(&probe);

        QCommandLineParser parser;
        configureParser(parser);
        parser.parse(probe.arguments());
        // addHelpOption() also registers --help-all. Both get the ordinary
        // help text here: the extended Qt-options form is reachable only
        // through process(), and the point of this block is to exit before
        // a display is touched, not to match that output.
        if (parser.isSet(QStringLiteral("help")) || parser.isSet(QStringLiteral("help-all"))) {
            parser.showHelp(0);
        }
        if (parser.isSet(QStringLiteral("version"))) {
            parser.showVersion();
        }
    }

    // Exit with kExitSessionNotServed, which the unit does not restart, if
    // there is no usable Wayland display, rather than abort and dump core.
    // When systemd respawns us during the logout → SDDM handoff (or
    // autostarts us in a session that has no live wl_display), Qt's wayland
    // QPA can't create a wl_display, the xcb fallback also fails, and
    // QGuiApplication's constructor calls qFatal() → abort. The unit ends
    // failed either way, since it lists both SIGABRT and this status in
    // RestartPreventExitStatus=.
    // This is a Wayland-only daemon, so "no wayland socket" means "nothing to
    // do". Resolving the socket path handles the empty/unset WAYLAND_DISPLAY
    // case too (Qt's default "wayland-0"), which previously bypassed this guard
    // and let Qt abort. See queryPlasmaWorkspaceState() in daemon/plasma_workspace.cpp
    // for the full phantom-session analysis.
    //
    // Skip the probe entirely when WAYLAND_SOCKET is set: libwayland (and thus
    // Qt's wayland QPA) connects via that inherited fd and ignores
    // WAYLAND_DISPLAY, so there is a live connection even though no socket path
    // resolves. Probing a path in that case would make us exit on a perfectly
    // usable session.
    if (qEnvironmentVariableIsEmpty("WAYLAND_SOCKET")) {
        const QByteArray waylandDisplay = qgetenv("WAYLAND_DISPLAY");
        const QByteArray runtimeDir = qgetenv("XDG_RUNTIME_DIR");
        const QString socketPath = resolveWaylandSocketPath(waylandDisplay, runtimeDir);
        if (socketPath.isEmpty() || !QFile::exists(socketPath)) {
            std::fprintf(stderr,
                         "plasmazones: no usable Wayland display (WAYLAND_DISPLAY=\"%s\", "
                         "resolved socket \"%s\") — wayland session not available, "
                         "exiting without asking to be restarted\n",
                         waylandDisplay.constData(),
                         socketPath.isEmpty() ? "<none>" : socketPath.toLocal8Bit().constData());
            return kExitSessionNotServed;
        }
    }

    // Taken before the first change below and compared after the last, so the
    // spawn sites can give the editor and the settings app the environment
    // this process inherited (see kDaemonPrivateEnvVars).
    const QList<std::optional<QByteArray>> inheritedPrivateEnv = inheritedPrivateEnvironment();

    // Opt out of MangoHud's implicit Vulkan layer injection. MangoHud's
    // implicit_layer manifest attaches whenever MANGOHUD=1 is in the
    // environment (e.g. set globally for games), and its NVIDIA stat-polling
    // thread costs ~30% CPU continuously inside this daemon — we are a
    // background service, not a game client. MANGOHUD is unset and
    // DISABLE_MANGOHUD is set:
    // MANGOHUD=0 alone is not enough on all manifest versions; the explicit
    // DISABLE_MANGOHUD opt-out is honored regardless of MANGOHUD's value.
    // Must run before QVulkanInstance::create() in vulkansupport.cpp.
    qunsetenv("MANGOHUD");
    qputenv("DISABLE_MANGOHUD", "1");

    // Use the simple animation driver (Qt 6.5+). With the threaded scene-
    // graph render loop and TWO OR MORE QQuickWindows, Qt 6's default
    // animation driver explicitly "falls back to the system timer based
    // approach" — and crucially, every render-thread sync phase blocks the
    // GUI thread for the duration of that window's polishAndSync. Animation
    // ticks driven by GUI-thread QTimers (which is how SurfaceAnimator's
    // m_driverTimer works) miss frames whenever the GUI thread is parked
    // in a sibling window's sync — the visible symptom is one popup's
    // shader fly-in pausing partway through while another popup mounts and
    // animates, then jumping forward when the GUI thread resumes.
    //
    // QSG_USE_SIMPLE_ANIMATION_DRIVER=1 swaps the multi-window-fallback
    // timer driver for QElapsedTimer-based timing, decoupling the
    // animation tick cadence from the per-window vsync / sync handshake.
    // Documented in the Qt Quick Scene Graph manual page as the supported
    // way to address exactly this multi-window stutter pattern. Must be
    // set before QGuiApplication is constructed.
    qputenv("QSG_USE_SIMPLE_ANIMATION_DRIVER", "1");

    // Register our layer-shell QPA plugin before QGuiApplication
    PhosphorWayland::registerLayerShellPlugin();

    // The last of the private changes: registerLayerShellPlugin() sets
    // QT_WAYLAND_SHELL_INTEGRATION when it was empty and a Wayland display or
    // socket is there.
    const PrivateEnvironmentChanges privateEnvChanges = privateEnvironmentChanges(inheritedPrivateEnv);

    // vulkanInstance is declared BEFORE app so C++ destruction order guarantees
    // it outlives QGuiApplication (and all QQuickWindows that reference it).
    // It is created further down, with the rendering backend, once
    // config.json has been migrated (see the block after ensureJsonConfig()).
#if QT_CONFIG(vulkan)
    QVulkanInstance vulkanInstance;
#endif
    // The GPU pin, read from disk before the migration below has run. GL
    // device choice happens when Mesa opens the DRI screen during platform
    // init, so DRI_PRIME must be exported before the app object, which is
    // before the migration can run. Reading early is safe for this key:
    // Rendering.Gpu postdates the v1 schema, and no migration step renames or
    // moves it, so every config that has it already holds it at its current
    // place. Applied on the Vulkan path too: it only steers the GL loader, and
    // covers the case where Vulkan falls back to OpenGL. The backend is NOT
    // read here: see the block after ensureJsonConfig().
    const QString gpuDevice = PlasmaZones::ConfigDefaults::readRenderingConfigFromDisk().gpuDevice;
    PlasmaZones::applyOpenGlGpuPreference(gpuDevice);

    QGuiApplication app(argc, argv);

    // Publish the daemon-private variables changed above, so spawn sites in
    // plasmazones_core (e.g. LayoutAdaptor::launchEditor) can undo them for a
    // child through PlasmaZones::childProcessEnvironment() without a link
    // dependency on this target. The GPU-preference variables are published
    // with the rendering backend below, after the Vulkan instance has
    // exported its own.
    app.setProperty(PlasmaZones::PDaemonEnvIntroducedVarsProperty, privateEnvChanges.introduced);
    app.setProperty(PlasmaZones::PDaemonEnvInheritedVarsProperty, privateEnvChanges.inherited);

    PlasmaZones::loadTranslations(&app);

    // Register D-Bus struct types for typed signal/method exchange
    PhosphorProtocol::registerWireTypes();

    // Register metatype for QVariant storage (LayerSurface stores itself
    // as a QWindow dynamic property via QVariant::fromValue).
    qRegisterMetaType<PhosphorWayland::LayerSurface*>();

    // Verify the layer-shell QPA plugin loaded successfully. If not, overlays will
    // be created as xdg_toplevel (wrong stacking/anchoring) — warn loudly.
    // Not gated on WAYLAND_DISPLAY: the guard at the top of main() admits a
    // session with that variable empty or unset (the "wayland-0" default, or a
    // connection inherited through WAYLAND_SOCKET), and it needs the warning
    // as much as any other.
    if (!PhosphorWayland::LayerSurface::isSupported()) {
        qCCritical(lcDaemon) << "Layer-shell QPA plugin did not initialize —"
                             << "overlays will use xdg_toplevel (wrong stacking/anchoring)."
                             << "Zone overlays will appear as regular windows (visible in taskbar,"
                             << "wrong z-order, no keyboard grab). This compositor may not support"
                             << "zwlr_layer_shell_v1 (e.g. GNOME/Mutter)."
                             << "Check that phosphorwayland-qpa.so is installed to Qt's"
                             << "wayland-shell-integration plugin directory.";
    }

    // Daemon must survive monitor power-off (DP disconnect destroys all overlay
    // windows; without this, Qt sees zero windows and calls quit()).
    app.setQuitOnLastWindowClosed(false);

    // Register ZoneShaderItem for QML
    // This enables RenderNodeOverlayContent.qml to use the GPU-accelerated
    // shader item (the item's ctor registers the ZoneLabelTexture metatype +
    // QImage converter).
    qmlRegisterType<PlasmaZones::ZoneShaderItem>("PlasmaZones", 1, 0, "ZoneShaderItem");
    // SurfaceShaderItem (the per-surface decoration layer) needs no
    // registration here: it ships in the org.phosphor.surface module linked
    // into this binary, beside the SurfaceDecoration.qml host that
    // OverlayService::applyDecoration drives on the OSD / popup surfaces. The
    // per-application-window host lives in the kwin-effect
    // (renderSurfaceChainComposite), not in this process.

    // Set up application metadata
    // applicationName is the KGlobalAccel component key — must match plasmazonesd.desktop
    app.setApplicationName(QString::fromLatin1(kApplicationName));
    app.setApplicationDisplayName(QStringLiteral("PlasmaZones"));
    app.setApplicationVersion(PlasmaZones::VERSION_STRING);
    app.setOrganizationName(QStringLiteral("plasmazones"));
    app.setOrganizationDomain(QStringLiteral("phosphor-works.org"));
    app.setDesktopFileName(QStringLiteral("org.phosphor_works.PlasmaZones"));
    app.setWindowIcon(QIcon::fromTheme(QStringLiteral("org.phosphor_works.PlasmaZones")));

    // Command line options. Same table the early help/version path at the top
    // of main() registers — see configureParser().
    QCommandLineParser parser;
    const DaemonOptions options = configureParser(parser);
    parser.process(app);

    // --log-file: redirect Qt message output to a file.
    // Opened here, as soon as the options are parsed and before the --debug
    // filter rules, so everything logged from this point on goes to the file.
    // What was logged above (the GPU-pin and layer-shell diagnostics) ran
    // before the parser and went to stderr. The static FILE*
    // and QMutex intentionally leak — they must outlive QCoreApplication for
    // shutdown messages.
    static FILE* logFile = nullptr;
    static QMutex* logMutex = nullptr;
    if (parser.isSet(options.logFile)) {
        const QByteArray pathLocal = parser.value(options.logFile).toLocal8Bit();
        // "e" is O_CLOEXEC: the daemon spawns the settings app and the editor,
        // and neither should inherit the log file's descriptor.
        logFile = fopen(pathLocal.constData(), "ae");
        // Read errno before anything else can change it. The warning below
        // builds a QDebug stream first, which may allocate.
        const int openErrno = errno;
        if (logFile) {
            logMutex = new QMutex;
            qInstallMessageHandler([](QtMsgType type, const QMessageLogContext& ctx, const QString& msg) {
                const QByteArray formatted = qFormatLogMessage(type, ctx, msg).toUtf8();
                QMutexLocker lock(logMutex);
                fprintf(logFile, "%s\n", formatted.constData());
                fflush(logFile);
            });
            fprintf(stderr, "plasmazonesd: logging to %s\n", pathLocal.constData());
        } else {
            qCWarning(PlasmaZones::lcDaemon)
                << "Failed to open log file:" << parser.value(options.logFile) << "-" << strerror(openErrno);
        }
    }

    if (parser.isSet(options.debug)) {
        // One rule per line. The daemon's own categories are plasmazones.*,
        // the placement engines and the libraries beside them log under
        // org.phosphor.*, and the other libraries it links log under names
        // that start with phosphor (phosphor.screens,
        // phosphorwayland.layersurface and the rest).
        QLoggingCategory::setFilterRules(QStringLiteral("plasmazones.*=true\norg.phosphor.*=true\nphosphor*=true"));
    }

    // Ensure single instance via D-Bus service name registration
    const QString serviceName = QString(PhosphorProtocol::Service::Name);
    QDBusConnection bus = QDBusConnection::sessionBus();

    // A missing session bus and an already-owned name both make registerService
    // return false, and they need opposite exits: no bus is a hard failure that
    // systemd should retry, a taken name is a duplicate start that must NOT
    // trigger Restart=on-failure. The return value alone does not separate them,
    // but QDBusConnection::lastError() does: an owned name leaves
    // QDBusError::NoError, a failed request leaves the error (the disconnect is
    // QDBusError::Disconnected). The registerService site below reads it. The
    // connection is checked here first because waitForKWin() needs a live bus
    // before the name is asked for at all.
    if (!bus.isConnected()) {
        qCCritical(PlasmaZones::lcDaemon) << "Cannot connect to the session D-Bus:" << bus.lastError().message()
                                          << "— the daemon places every window over D-Bus and cannot run without it."
                                          << "Check that DBUS_SESSION_BUS_ADDRESS is set and the bus is running.";
        return 1;
    }

    switch (waitForKWin(bus)) {
    case KWinPresence::Present:
        break;
    case KWinPresence::Absent:
        // kExitSessionNotServed, not 1: outside KWin this is not a failure for
        // systemd to retry, and the unit's RestartPreventExitStatus= says so.
        qCWarning(PlasmaZones::lcDaemon) << "org.kde.KWin is not on the session bus, so this is not a KWin session."
                                         << "plasmazonesd only runs under KWin; exiting.";
        return kExitSessionNotServed;
    case KWinPresence::Unknown:
        // Exit 1, not 0: the bus could not be asked, so nothing says this is
        // not a KWin session. waitForKWin() has logged the bus error.
        return 1;
    }

    // registerService does not queue: when another process owns the name it
    // fails at once, so this daemon never waits to take a session's placement
    // API over from another host.
    if (!bus.registerService(serviceName)) {
        // Read before any other call on this connection: every blocking call
        // sets or clears QDBusConnection::lastError(). NoError here means the
        // bus answered and another connection owns the name.
        const QDBusError registerError = bus.lastError();
        if (parser.isSet(options.replace)) {
            // Ask the incumbent to quit through Control.quit, then wait for the
            // name to come free. That needs an incumbent whose event loop still
            // answers: a wedged one never dispatches the call, is not displaced,
            // and the replace fails once the wait loop's budget is spent. The
            // name is also bus-activatable, and between the incumbent releasing
            // it and the next poll below it has no owner: a client call that
            // reaches the bus in that gap can activate another daemon that
            // takes the name first, and then this replace fails after its
            // budget with that daemon running.
            //
            // Learn the incumbent's pid FIRST, while it still owns the name: once
            // the name is ours the bus no longer knows who held it, and the wait
            // for that process to exit (below) needs it. When the incumbent let
            // go of the name between the failed registerService above and this
            // call, the bus answers "no owner" while the incumbent is still in
            // its shutdown saves, so the pid then comes from the host record
            // (recordedIncumbentPid(), which checks the record still describes
            // a live plasmazonesd). Not knowing is not a failure: the replace
            // goes on without the wait.
            pid_t incumbentPid = 0;
            if (QDBusConnectionInterface* const busInterface = bus.interface()) {
                const QDBusReply<uint> pidReply = busInterface->servicePid(serviceName);
                if (!pidReply.isValid()) {
                    qCDebug(PlasmaZones::lcDaemon)
                        << "--replace: could not learn the running instance's pid:" << pidReply.error().message();
                } else if (pidReply.value() > 0 && pidReply.value() <= uint(std::numeric_limits<pid_t>::max())
                           && qint64(pidReply.value()) != QCoreApplication::applicationPid()) {
                    // Zero and anything past pid_t would mean a process GROUP to
                    // kill(), and our own pid would never be seen to exit.
                    incumbentPid = pid_t(pidReply.value());
                }
            }
            if (incumbentPid <= 0) {
                incumbentPid = recordedIncumbentPid();
                if (incumbentPid > 0) {
                    qCDebug(PlasmaZones::lcDaemon)
                        << "--replace: the bus did not name the running instance; the host record names pid"
                        << incumbentPid;
                }
            }

            // The object path and interface must be the ones the daemon actually
            // exports: ControlAdaptor is Qt-parented to the Daemon, which is
            // registered at Service::ObjectPath, so the shutdown verb lives at
            // org.phosphor_works.Placement.Control.quit on /org/phosphor_works/Placement. Addressing anything
            // else silently no-ops: the call is answered with an error reply
            // that nothing reads, and the retry loop below just burns its
            // budget while the old instance keeps the name.
            //
            // A bare method-call message, not a QDBusInterface: the proxy's
            // constructor makes a blocking Introspect call on the incumbent,
            // and against one that does not answer it sits on the default reply
            // timeout before the first line after it runs.
            QDBusMessage quitCall = QDBusMessage::createMethodCall(
                serviceName, QString(PhosphorProtocol::Service::ObjectPath),
                QString(PhosphorProtocol::Service::Interface::Control), QStringLiteral("quit"));
            // Asking a daemon to quit must never start one: with auto-start on,
            // the bus would activate the service for an incumbent that has just gone.
            quitCall.setAutoStartService(false);
            // asyncCall, NOT call(): an incumbent that does not answer would
            // hold a blocking call on the default 25s reply timeout before the
            // wait loop below even starts. Nothing reads the reply — the retry
            // loop is what actually detects that the name came free.
            bus.asyncCall(quitCall);
            bool registered = false;
            // 100, 200, 400, then 800ms x7 — 6.3s total.
            for (int attempt = 0; attempt < 10; ++attempt) {
                QThread::msleep(100 * (1 << qMin(attempt, 3)));
                if (bus.registerService(serviceName)) {
                    registered = true;
                    break;
                }
            }
            if (!registered) {
                qCCritical(PlasmaZones::lcDaemon) << "Failed to register D-Bus service after --replace";
                return 1;
            }
            // The name coming free does not mean the incumbent is done.
            // Daemon::stop() releases it BEFORE its shutdown save of the window
            // tracking state and its layout, assignment and settings saves, so
            // a successor that went straight on would load state the incumbent
            // is still writing and later save over it. Wait for the process
            // itself, which is past every one of those saves once it is gone.
            if (incumbentPid <= 0) {
                qCDebug(PlasmaZones::lcDaemon)
                    << "--replace: the previous instance's pid is unknown, so not waiting for it to exit";
            } else if (!waitForProcessExit(incumbentPid)) {
                // Warn and carry on, deliberately: no signal is sent to a
                // process that is still saving.
                qCWarning(PlasmaZones::lcDaemon)
                    << "--replace: the previous instance released the bus name but had not exited after"
                    << kIncumbentExitBudgetMs << "ms, pid=" << incumbentPid
                    << "- starting anyway; state it has not finished saving may be missed or overwritten";
            }
        } else if (registerError.type() == QDBusError::NoError) {
            qCWarning(PlasmaZones::lcDaemon) << "Already running (D-Bus name taken)";
            return 0;
        } else {
            // The request itself failed, so nothing says another instance is
            // running. Exit 1 so a supervised start is retried.
            qCCritical(PlasmaZones::lcDaemon) << "Failed to register D-Bus service=" << serviceName
                                              << "error=" << registerError.message() << "type=" << registerError.type();
            return 1;
        }
    }

    // Record this daemon as the user's placement host. A settings module that
    // reads the record through PhosphorConfig::HostDirectory::resolve() then
    // edits ConfigDefaults::configFilePath(). While this process owns the bus
    // name, the record's pid matches the owner, so the record wins on any
    // desktop and the module can also ask the process to reload. In a later
    // session that starts with the daemon stopped, nothing owns the name and
    // the record still names the file, but only where that session's
    // XDG_CURRENT_DESKTOP names a desktop the descriptor serves (KDE for
    // plasmazones.json) or names none: a host's record does not cross
    // desktops. KDE covers every Plasma session, desktop and mobile alike:
    // startplasma sets XDG_CURRENT_DESKTOP to exactly "KDE" for each of them.
    // The record is also what --replace falls back on to find the incumbent
    // (recordedIncumbentPid()). The id is PlacementHostId, the one in
    // data/placement-hosts/plasmazones.json, which the package installs.
    if (!PhosphorConfig::HostDirectory(QString(PhosphorProtocol::Service::HostApi))
             .writeRecord(QString(PlasmaZones::PlacementHostId), PlasmaZones::ConfigDefaults::configFilePath(),
                          QCoreApplication::applicationPid())) {
        // Not fatal: placement does not read the record. HostDirectory has
        // logged why the write failed.
        qCWarning(PlasmaZones::lcDaemon) << "The placement host record was not written";
    }

    // Set up signal handling for clean shutdown.
    //
    // Installed BEFORE the Daemon is constructed so the whole startup window is
    // covered, and correct there because the handler only writes to a pipe: a
    // signal arriving while init() is still running leaves its byte buffered and
    // the notifier fires as soon as exec() starts, so the shutdown is honoured
    // instead of being lost. The old handler called quit() directly, which is a
    // no-op on a loop that has not started, and then let startup continue into
    // exec() with stop() already applied.
    // O_CLOEXEC so the fds do not leak into the processes the daemon spawns
    // (the settings app via QProcess::startDetached, the editor via
    // LayoutAdaptor). O_NONBLOCK so a hypothetically full pipe makes the
    // handler's write() fail rather than block inside signal context, which is
    // the one thing this whole mechanism exists to avoid.
    if (::pipe2(g_signalPipe, O_CLOEXEC | O_NONBLOCK) == 0) {
        auto* signalNotifier = new QSocketNotifier(g_signalPipe[0], QSocketNotifier::Read, &app);
        QObject::connect(signalNotifier, &QSocketNotifier::activated, &app, [](QSocketDescriptor socket) {
            unsigned char signum = 0;
            const ssize_t got = ::read(static_cast<int>(socket), &signum, 1);
            if (got != 1) {
                return;
            }
            // Warning, not info: lcDaemon prints info by default, but a logging
            // rule that silences info (a QT_LOGGING_RULES or qtlogging.ini
            // "*.info=false") must not take the reason the daemon exited out
            // of the journal with it.
            qCWarning(PlasmaZones::lcDaemon) << "Received signal" << signum << "- shutting down";
            // Hand the signals back to the kernel before quitting. Everything
            // after exec() returns — Daemon::stop() with its blocking D-Bus
            // round trips and thread-pool join, then the window teardown —
            // runs with no event loop reading this pipe, so a second SIGTERM in
            // that window would be written and never acted on, leaving SIGKILL
            // as the only way out. Restoring the inherited dispositions makes
            // the first signal graceful and a second one act as it would have
            // without the daemon's handler, which is what an operator pressing
            // Ctrl-C twice expects.
            restoreInheritedSignalDispositions();
            QCoreApplication::quit();
        });
        installSignalHandler();
    } else {
        // Read errno first: the warning below builds a QDebug stream before it
        // reaches strerror(), and that may allocate.
        const int pipeErrno = errno;
        // Without the pipe there is no safe way to hop out of signal context, so
        // leave the default dispositions in place: an abrupt terminate on SIGTERM
        // is worse than a graceful stop but far better than a handler that
        // deadlocks the process mid-teardown.
        qCWarning(PlasmaZones::lcDaemon)
            << "Failed to create the signal self-pipe:" << strerror(pipeErrno)
            << "- falling back to default signal handling, so shutdown will not be graceful";
    }

    // Bring config.json up to the current schema before the Daemon is
    // constructed. ConfigMigration::ensureJsonConfig() converts an INI config
    // on an upgrade, runs the schema migration chain, and runs the filesystem
    // finalizers, which it does on every start and not only on a migrating
    // one. The editor and settings app call it too, in case they start before
    // the daemon, and the callee takes a cross-process QLockFile around the
    // read-migrate-write sequence against exactly that concurrent start.
    //
    // Load-bearing HERE specifically, and it must stay ahead of the Daemon
    // constructor: the daemon injects its own backend (createDefaultConfigBackend()
    // in daemon.cpp's member-init list), so it takes the Settings ctor overload
    // that does NOT route through migrateAndCreateOwnedBackend(). Only the
    // backend-owning overloads migrate on construction. Drop this call and the
    // daemon opens an unmigrated config.
    if (!PlasmaZones::ConfigMigration::ensureJsonConfig()) {
        qCWarning(PlasmaZones::lcDaemon)
            << "Config migration reported a failure — continuing with whatever the backend can load,"
            << "which may mean defaults for settings the old config held";
    }

    // Read the rendering backend and probe for the Vulkan loader library.
    // Here, after ensureJsonConfig() and not before QGuiApplication with the
    // GPU pin, because the key has moved: a v1 config.json holds it as
    // Rendering.RenderingBackend, which migrateV1ToV2 renames to
    // Rendering.Backend, so a read ahead of the migration missed it on the
    // start that migrates and ran that session on the default backend. This
    // is still early enough: probeAndSetGraphicsApi() calls
    // QQuickWindow::setGraphicsApi(), which Qt requires before the first
    // QQuickWindow is constructed, not before the app object, and the first
    // one is built by the Daemon below. The instance is created right after,
    // by createAndRegisterVulkanInstance(), which falls back to OpenGL the
    // same way when it fails. Both now run with the bus name already held, so
    // a Vulkan driver that hangs device enumeration (a driver upgraded without
    // a reboot) leaves callers of org.phosphor_works.Placement waiting on
    // their reply timeout rather than finding no owner.
    {
        const QString backend = PlasmaZones::ConfigDefaults::readRenderingConfigFromDisk().backend;
        const bool useVulkan = PlasmaZones::probeAndSetGraphicsApi(backend);
        if (!useVulkan && backend == QLatin1String("vulkan")) {
            // probeAndSetGraphicsApi answers false for a configured "vulkan"
            // either because the loader library did not load or because this
            // Qt was built without Vulkan. Only the first is something a
            // package fixes, so each gets its own message.
#if QT_CONFIG(vulkan)
            qCCritical(PlasmaZones::lcDaemon) << "Vulkan library not found — falling back to OpenGL."
                                              << "Install vulkan-icd-loader or equivalent for your distro.";
#else
            qCCritical(PlasmaZones::lcDaemon) << "This Qt build has no Vulkan support — falling back to OpenGL."
                                              << "The Vulkan rendering backend needs a Qt built with Vulkan enabled.";
#endif
        }
        // The parenthetical reports the probe OUTCOME, not the config string:
        // a configured "vulkan" whose library failed to load is already on
        // OpenGL at this point (probeAndSetGraphicsApi set it during the probe).
        qCInfo(PlasmaZones::lcDaemon) << "Rendering backend:" << backend
                                      << (useVulkan                                ? "(Vulkan)"
                                              : backend == QLatin1String("vulkan") ? "(OpenGL fallback)"
                                              : backend == QLatin1String("opengl") ? "(OpenGL)"
                                                                                   : "(Qt default)");

        // Store the instance pointer as a dynamic property (PVulkanInstanceProperty)
        // so the OverlayService constructor can read it and hand it to its
        // PhosphorSurfaces::SurfaceManager, which calls setVulkanInstance() on each
        // QQuickWindow it creates.
#if QT_CONFIG(vulkan)
        qRegisterMetaType<QVulkanInstance*>();
        if (useVulkan) {
            // gpuDevice is the pin DRI_PRIME was exported from, so the Vulkan
            // pick and the GL one name the same device.
            if (PlasmaZones::createAndRegisterVulkanInstance(vulkanInstance, app, gpuDevice)) {
                qCInfo(PlasmaZones::lcDaemon) << "Vulkan instance created successfully";
            } else {
                qCCritical(PlasmaZones::lcDaemon)
                    << "Vulkan unavailable (instance creation failed or no enumerable GPU) — falling back to OpenGL."
                    << "Check that Vulkan drivers are installed and match the running kernel module"
                    << "(a GPU driver upgrade without a reboot leaves a userspace/kernel version skew that"
                    << "breaks device enumeration).";
            }
        }
#endif
    }

    // Publish the GPU-preference env var names this process exported (and the
    // ones it cleared, with values) so the spawn sites can scrub and restore
    // child environments through PlasmaZones::childProcessEnvironment().
    // After createAndRegisterVulkanInstance(), which exports
    // QT_VK_PHYSICAL_DEVICE_INDEX, and before the Daemon, whose spawn sites
    // read them.
    app.setProperty(PlasmaZones::PGpuExportedVarsProperty, PlasmaZones::exportedGpuPreferenceVariables());
    app.setProperty(PlasmaZones::PGpuClearedVarsProperty, PlasmaZones::clearedGpuPreferenceVariables());

    // Create and start daemon
    Daemon daemon;

    if (!daemon.init()) {
        qCCritical(PlasmaZones::lcDaemon) << "Failed to initialize daemon";
        // The event loop never starts on this path, so nothing will read the
        // signal pipe while the Daemon and the application are torn down.
        restoreInheritedSignalDispositions();
        return 1;
    }

    qCInfo(PlasmaZones::lcDaemon) << "Daemon started";
    daemon.start();

    int result = app.exec();

    // The notifier slot restores the dispositions only when a signal ended the
    // loop. Control.quit ends it too, and gets here with signalHandler still
    // installed, so restore on every exit: the teardown below is the window
    // the slot's comment describes.
    restoreInheritedSignalDispositions();

    daemon.stop();

    // Close all remaining windows before QGuiApplication teardown.
    // The NVIDIA Vulkan ICD (driver 595.x+) crashes in vkDestroyInstance during
    // dlclose when GPU worker threads are still running. Explicitly closing windows
    // here lets the Wayland platform plugin tear down EGL/Vulkan surfaces while the
    // event loop is still partially intact, before ~QGuiApplication triggers the
    // problematic dlclose path. This is an NVIDIA driver bug — this workaround
    // reduces (but may not eliminate) the crash window.
    const auto topLevels = QGuiApplication::topLevelWindows();
    for (QWindow* w : topLevels) {
        w->destroy();
    }

    return result;
}
