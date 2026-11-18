// SPDX-FileCopyrightText: 2026 fuddlesworth
// SPDX-License-Identifier: GPL-3.0-or-later

#include "kwinworkspacehost.h"

#include "common/kwinperoutputdesktops.h"
#include "core/platform/logging.h"
#include "daemon/controllers/shortcutmanager.h"

#include <QDBusConnection>
#include <QDBusMessage>
#include <QDBusPendingCall>
#include <QDir>
#include <QFile>
#include <QFileInfo>
#include <QKeySequence>
#include <QSettings>
#include <QStandardPaths>

#include <optional>

namespace PlasmaZones {

namespace {
QString kwinShortcutBackupPath()
{
    // StateLocation is already app-scoped. .ini because every reader and
    // writer opens it as QSettings::IniFormat.
    return QStandardPaths::writableLocation(QStandardPaths::StateLocation)
        + QStringLiteral("/kwin-shortcut-backup.ini");
}

void askKWinToReconfigure()
{
    QDBusConnection::sessionBus().asyncCall(
        QDBusMessage::createMethodCall(QStringLiteral("org.kde.KWin"), QStringLiteral("/KWin"),
                                       QStringLiteral("org.kde.KWin"), QStringLiteral("reconfigure")));
}

/// The stock KWin desktop-switch actions the rebind toggle takes over.
const QStringList& kwinDesktopActions()
{
    static const QStringList actions{
        QStringLiteral("Switch One Desktop Up"),          QStringLiteral("Switch One Desktop Down"),
        QStringLiteral("Switch One Desktop to the Left"), QStringLiteral("Switch One Desktop to the Right"),
        QStringLiteral("Walk Through Desktops"),          QStringLiteral("Walk Through Desktops (Reverse)")};
    return actions;
}
} // namespace

KWinWorkspaceHost::KWinWorkspaceHost(ShortcutManager* shortcutManager)
    : m_shortcutManager(shortcutManager)
{
}

bool KWinWorkspaceHost::supportsDynamicWorkspaces() const
{
#ifdef USE_KDE_FRAMEWORKS
    return true;
#else
    return false;
#endif
}

bool KWinWorkspaceHost::perOutputDesktopsConfigured() const
{
    return kwinPerOutputVirtualDesktopsEnabled();
}

bool KWinWorkspaceHost::enablePerOutputDesktops()
{
    if (!enableKWinPerOutputVirtualDesktops()) {
        qCWarning(lcDaemon) << "could not turn on KWin per-output virtual desktops (the kwinrc entry is locked or "
                               "kwinrc cannot be written)";
        return false;
    }
    askKWinToReconfigure();
    qCInfo(lcDaemon) << "wrote PerOutputVirtualDesktops=true to kwinrc and asked KWin to reconfigure (user consent)";
    return true;
}

void KWinWorkspaceHost::takeOverStockDesktopShortcuts()
{
    if (!m_shortcutManager) {
        return;
    }
    const QString path = kwinShortcutBackupPath();
    if (!QDir().mkpath(QFileInfo(path).absolutePath())) {
        qCWarning(lcDaemon) << "cannot create the state directory for" << path
                            << "- leaving KWin's desktop-switch chords alone";
        return;
    }
    QSettings backup(path, QSettings::IniFormat);
    for (const QString& action : kwinDesktopActions()) {
        // A backup entry already present means the chord is ALREADY
        // stolen, so nothing re-reads it. That also means a user who
        // re-binds the stock chord by hand while we hold the backup keeps
        // it: re-stealing would fight the user's explicit choice.
        if (backup.contains(action)) {
            continue;
        }
        const std::optional<QList<QKeySequence>> current =
            m_shortcutManager->foreignShortcuts(QStringLiteral("kwin"), action);
        if (!current) {
            // The query FAILED: a backend without foreign rebinding (the
            // portal, the D-Bus trigger fallback) or kglobalacceld
            // unreachable. That is not "unbound", and stealing on the
            // strength of it would clear a live chord with no backup. The
            // rest of the actions would fail the same way, so the pass
            // stops here and the next one tries again.
            qCInfo(lcDaemon) << "cannot read KWin's desktop-switch shortcuts through this shortcut backend; "
                                "leaving them alone";
            return;
        }
        if (current->isEmpty()) {
            continue; // genuinely unbound; nothing to steal or restore
        }
        // The backup is written and FLUSHED before the clear, per action,
        // and the clear only happens once the flush is known to have
        // landed: a clear with no backup on disk loses the user's chord
        // for good. A backup for a clear that then failed is harmless,
        // the restore simply rewrites a value that is already live.
        backup.setValue(action, QKeySequence::listToString(*current, QKeySequence::PortableText));
        backup.sync();
        if (backup.status() != QSettings::NoError) {
            qCWarning(lcDaemon) << "cannot write" << path << "- leaving KWin's desktop-switch chords alone";
            return;
        }
        if (!m_shortcutManager->setForeignShortcuts(QStringLiteral("kwin"), action, {})) {
            qCWarning(lcDaemon) << "could not clear KWin shortcut" << action
                                << "- its backup entry stands and the next restore rewrites what is already live";
        }
    }
}

/// Give KWin its stolen desktop-switch chords back. Runs on every stop
/// (logout included) and on every start that leaves the feature off or
/// dormant, so a takeover persisted by an earlier session is always undone.
/// Consumes the backup entry by entry, and only entries that are settled:
/// restored, or found re-bound by hand. Dropping an entry after a failed
/// call (kglobalaccel unreachable) would strand the user's chord on "none"
/// forever, while keeping it means the next restore tries again.
void KWinWorkspaceHost::restoreStockDesktopShortcuts()
{
    ShortcutManager* const shortcutManager = m_shortcutManager;
    if (!shortcutManager || !QFile::exists(kwinShortcutBackupPath())) {
        return;
    }
    QSettings backup(kwinShortcutBackupPath(), QSettings::IniFormat);
    const QStringList backedUp = backup.allKeys();
    for (const QString& action : backedUp) {
        const std::optional<QList<QKeySequence>> current =
            shortcutManager->foreignShortcuts(QStringLiteral("kwin"), action);
        if (!current) {
            // This backend cannot read KWin's bindings (or kglobalacceld is
            // unreachable), so it cannot restore them either. Every entry
            // stays for a pass that can.
            qCInfo(lcDaemon) << "cannot reach KWin's shortcuts to restore the desktop-switch chords; keeping"
                             << backedUp.size() << "backup entries";
            break;
        }
        if (!current->isEmpty()) {
            // The chord was cleared when it was taken, so a binding now means
            // the user set one by hand while the backup was held. Theirs wins.
            qCInfo(lcDaemon) << "KWin shortcut" << action
                             << "was re-bound while PlasmaZones held it; keeping that binding";
            backup.remove(action);
            continue;
        }
        // listFromString also reads the pre-multi-sequence single-string
        // format (a one-element list).
        const QList<QKeySequence> sequences = QKeySequence::listFromString(backup.value(action).toString());
        if (shortcutManager->setForeignShortcuts(QStringLiteral("kwin"), action, sequences)) {
            backup.remove(action);
        } else {
            qCWarning(lcDaemon) << "could not restore KWin shortcut" << action << "- keeping its backup entry";
        }
    }
    backup.sync();
}

} // namespace PlasmaZones
