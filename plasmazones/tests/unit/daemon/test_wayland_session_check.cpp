// SPDX-FileCopyrightText: 2026 fuddlesworth
// SPDX-License-Identifier: GPL-3.0-or-later

/**
 * @file test_wayland_session_check.cpp
 * @brief Regression: the daemon startup guard in main() must resolve the
 *        Wayland socket path for an empty/unset WAYLAND_DISPLAY too.
 *
 * Why this exists: plasmazonesd is Wayland-only. When systemd respawns it
 * during a logout → SDDM handoff (or autostarts it in a session with no live
 * wl_display), Qt's wayland QPA aborts in the QGuiApplication constructor via
 * qFatal() → SIGABRT → core dump. The guard in main() exits early with
 * kExitSessionNotServed (which plasmazones.service does not restart) when no
 * usable Wayland socket exists, turning the crash into a clean exit so
 * Restart=on-failure does not loop.
 *
 * The original guard only acted when WAYLAND_DISPLAY was *set but its socket
 * was missing*. Observed crashes (CachyOS bug report) showed WAYLAND_DISPLAY
 * *empty* ("could not connect to display "), which bypassed the guard entirely
 * and let Qt abort. resolveWaylandSocketPath() now resolves the empty case to
 * Qt's default "wayland-0" so the guard's file-existence check can fire. These
 * tests pin that resolution so the gap cannot silently reopen.
 */

#include "daemon/waylandsessioncheck.h"

#include <PhosphorWayland/LayerShellPluginLoader.h>

#include <QByteArray>
#include <QTemporaryDir>
#include <QTest>

#include <optional>

namespace {

/// Sets or unsets one environment variable for a scope and puts back what was
/// there before, so a case cannot leak into the next.
class ScopedEnv
{
public:
    ScopedEnv(const char* name, std::optional<QByteArray> value)
        : m_name(name)
        , m_old(qEnvironmentVariableIsSet(name) ? std::optional<QByteArray>(qgetenv(name)) : std::nullopt)
    {
        apply(value);
    }
    ~ScopedEnv()
    {
        apply(m_old);
    }
    ScopedEnv(const ScopedEnv&) = delete;
    ScopedEnv& operator=(const ScopedEnv&) = delete;

private:
    void apply(const std::optional<QByteArray>& value)
    {
        if (value) {
            qputenv(m_name, *value);
        } else {
            qunsetenv(m_name);
        }
    }
    const char* m_name;
    std::optional<QByteArray> m_old;
};

} // namespace

using namespace PlasmaZones;

class TestWaylandSessionCheck : public QObject
{
    Q_OBJECT

private Q_SLOTS:
    // An absolute WAYLAND_DISPLAY is used verbatim, ignoring XDG_RUNTIME_DIR.
    void absoluteDisplayUsedVerbatim()
    {
        QCOMPARE(resolveWaylandSocketPath("/custom/run/wayland-9", "/run/user/1000"),
                 QStringLiteral("/custom/run/wayland-9"));
        // Even with no runtime dir, an absolute path stands on its own.
        QCOMPARE(resolveWaylandSocketPath("/custom/run/wayland-9", QByteArray()),
                 QStringLiteral("/custom/run/wayland-9"));
    }

    // A relative WAYLAND_DISPLAY resolves against XDG_RUNTIME_DIR.
    void relativeDisplayResolvesAgainstRuntimeDir()
    {
        QCOMPARE(resolveWaylandSocketPath("wayland-1", "/run/user/1000"), QStringLiteral("/run/user/1000/wayland-1"));
    }

    // THE REGRESSION: an empty/unset WAYLAND_DISPLAY must fall back to Qt's
    // default "wayland-0" display name so the guard probes a real path instead
    // of doing nothing and letting QGuiApplication abort.
    void emptyDisplayFallsBackToWayland0()
    {
        QCOMPARE(resolveWaylandSocketPath(QByteArray(), "/run/user/952"), QStringLiteral("/run/user/952/wayland-0"));
        // Explicitly-empty (set to "") behaves identically to unset.
        QCOMPARE(resolveWaylandSocketPath(QByteArray(""), "/run/user/952"), QStringLiteral("/run/user/952/wayland-0"));
    }

    // With a relative/empty display name and no XDG_RUNTIME_DIR there is no
    // session — an empty path tells the guard to exit cleanly.
    void noRuntimeDirYieldsEmptyForRelativeOrEmptyDisplay()
    {
        QVERIFY(resolveWaylandSocketPath(QByteArray(), QByteArray()).isEmpty());
        QVERIFY(resolveWaylandSocketPath("wayland-0", QByteArray()).isEmpty());
    }

    // The guard above lets a process with only WAYLAND_SOCKET through
    // (libwayland connects through that fd ahead of any display name), so
    // the layer-shell plugin must be registered for it too. Without the
    // WAYLAND_SOCKET arm in registerLayerShellPlugin() nothing here names a
    // display or a socket, and every overlay came up as an xdg_toplevel.
    void layerShellPluginRegisteredForWaylandSocketAlone()
    {
        QTemporaryDir runtimeDir; // empty: no wayland-0 or wayland-1 socket
        QVERIFY(runtimeDir.isValid());
        ScopedEnv integration("QT_WAYLAND_SHELL_INTEGRATION", std::nullopt);
        ScopedEnv display("WAYLAND_DISPLAY", std::nullopt);
        ScopedEnv runtime("XDG_RUNTIME_DIR", runtimeDir.path().toLocal8Bit());
        ScopedEnv socket("WAYLAND_SOCKET", QByteArrayLiteral("3"));

        PhosphorWayland::registerLayerShellPlugin();

        QCOMPARE(qgetenv("QT_WAYLAND_SHELL_INTEGRATION"), QByteArrayLiteral("phosphorwayland"));
    }

    // And with no connection of any kind it registers nothing, so a headless
    // process keeps Qt's own choice.
    void layerShellPluginNotRegisteredWithoutAConnection()
    {
        QTemporaryDir runtimeDir;
        QVERIFY(runtimeDir.isValid());
        ScopedEnv integration("QT_WAYLAND_SHELL_INTEGRATION", std::nullopt);
        ScopedEnv display("WAYLAND_DISPLAY", std::nullopt);
        ScopedEnv runtime("XDG_RUNTIME_DIR", runtimeDir.path().toLocal8Bit());
        ScopedEnv socket("WAYLAND_SOCKET", std::nullopt);

        PhosphorWayland::registerLayerShellPlugin();

        QVERIFY(!qEnvironmentVariableIsSet("QT_WAYLAND_SHELL_INTEGRATION"));
    }
};

QTEST_MAIN(TestWaylandSessionCheck)
#include "test_wayland_session_check.moc"
