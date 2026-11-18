// SPDX-FileCopyrightText: 2026 fuddlesworth
// SPDX-License-Identifier: GPL-3.0-or-later

/**
 * @file test_kwin_workspace_host.cpp
 * @brief KWinWorkspaceHost's takeover of KWin's stock desktop-switch chords:
 *        a bound chord is backed up and cleared, the restore puts it back and
 *        consumes the backup, a chord the user re-bound by hand while it was
 *        held is theirs, and a backend that cannot read KWin's shortcuts
 *        takes nothing and keeps the backup for a pass that can.
 */

#include "daemon/controllers/shortcutmanager.h"
#include "daemon/host/kwinworkspacehost.h"

#include "config/settings.h"
#include "helpers/IsolatedConfigGuard.h"

#include <PhosphorShortcuts/IBackend.h>

#include <QHash>
#include <QKeySequence>
#include <QSettings>
#include <QStandardPaths>
#include <QTemporaryDir>
#include <QTest>

#include <memory>
#include <optional>

using namespace PlasmaZones;
using PlasmaZones::TestHelpers::IsolatedConfigGuard;

namespace {

/// A backend that holds KWin's bindings in memory, through the foreign-rebind
/// pass-through KGlobalAccelBackend implements.
class ForeignBackend : public PhosphorShortcuts::IBackend
{
    Q_OBJECT
public:
    explicit ForeignBackend(QHash<QString, QList<QKeySequence>>* kwin, const bool* readable)
        : m_kwin(kwin)
        , m_readable(readable)
    {
    }

    void registerShortcut(const QString&, const QKeySequence&, const QKeySequence&, const QString&, bool) override
    {
    }
    void updateShortcut(const QString&, const QKeySequence&, const QKeySequence&) override
    {
    }
    void unregisterShortcut(const QString&) override
    {
    }
    void flush() override
    {
        Q_EMIT ready();
    }

    bool setForeignShortcuts(const QString& componentName, const QString& actionName,
                             const QList<QKeySequence>& sequences) override
    {
        if (!*m_readable || componentName != QLatin1String("kwin")) {
            return false;
        }
        m_kwin->insert(actionName, sequences);
        return true;
    }

    std::optional<QList<QKeySequence>> foreignShortcuts(const QString& componentName,
                                                        const QString& actionName) const override
    {
        if (!*m_readable || componentName != QLatin1String("kwin")) {
            return std::nullopt;
        }
        return m_kwin->value(actionName);
    }

private:
    QHash<QString, QList<QKeySequence>>* m_kwin;
    const bool* m_readable;
};

const QString SwitchUp = QStringLiteral("Switch One Desktop Up");
const QString SwitchDown = QStringLiteral("Switch One Desktop Down");
const QString WalkThrough = QStringLiteral("Walk Through Desktops");

} // namespace

class TestKWinWorkspaceHost : public QObject
{
    Q_OBJECT

private Q_SLOTS:
    void init()
    {
        m_guard = std::make_unique<IsolatedConfigGuard>();
        // The backup lives in the state directory, which the guard leaves alone.
        m_stateDir = std::make_unique<QTemporaryDir>();
        QVERIFY(m_stateDir->isValid());
        qputenv("XDG_STATE_HOME", m_stateDir->path().toUtf8());
        m_kwin = {
            {SwitchUp, {QKeySequence(QStringLiteral("Meta+Ctrl+Up"))}},
            {SwitchDown, {QKeySequence(QStringLiteral("Meta+Ctrl+Down"))}},
            {WalkThrough, {}},
        };
        m_readable = true;
        m_settings = std::make_unique<Settings>();
        m_manager = std::make_unique<ShortcutManager>(m_settings.get());
        m_manager->setBackendForTesting(std::make_unique<ForeignBackend>(&m_kwin, &m_readable));
        m_manager->registerShortcuts();
        m_host = std::make_unique<KWinWorkspaceHost>(m_manager.get());
    }

    void cleanup()
    {
        m_host.reset();
        m_manager.reset();
        m_settings.reset();
        qunsetenv("XDG_STATE_HOME");
        m_stateDir.reset();
        m_guard.reset();
    }

    void takeoverBacksUpAndClearsThenRestoreGivesBack()
    {
        m_host->takeOverStockDesktopShortcuts();
        QVERIFY(m_kwin.value(SwitchUp).isEmpty());
        QVERIFY(m_kwin.value(SwitchDown).isEmpty());
        QSettings backup(backupPath(), QSettings::IniFormat);
        QVERIFY(backup.contains(SwitchUp));
        QVERIFY(backup.contains(SwitchDown));
        QVERIFY2(!backup.contains(WalkThrough), "an unbound chord has nothing to back up");

        m_host->restoreStockDesktopShortcuts();
        QCOMPARE(m_kwin.value(SwitchUp), QList<QKeySequence>{QKeySequence(QStringLiteral("Meta+Ctrl+Up"))});
        QCOMPARE(m_kwin.value(SwitchDown), QList<QKeySequence>{QKeySequence(QStringLiteral("Meta+Ctrl+Down"))});
        QSettings after(backupPath(), QSettings::IniFormat);
        QVERIFY2(after.allKeys().isEmpty(), "the restore consumes every settled entry");
    }

    void aChordReboundWhileHeldIsTheUsers()
    {
        m_host->takeOverStockDesktopShortcuts();
        m_kwin.insert(SwitchUp, {QKeySequence(QStringLiteral("Meta+Alt+Up"))});
        // A second takeover does not re-read a chord it already holds.
        m_host->takeOverStockDesktopShortcuts();
        QCOMPARE(m_kwin.value(SwitchUp), QList<QKeySequence>{QKeySequence(QStringLiteral("Meta+Alt+Up"))});

        m_host->restoreStockDesktopShortcuts();
        QCOMPARE(m_kwin.value(SwitchUp), QList<QKeySequence>{QKeySequence(QStringLiteral("Meta+Alt+Up"))});
        QCOMPARE(m_kwin.value(SwitchDown), QList<QKeySequence>{QKeySequence(QStringLiteral("Meta+Ctrl+Down"))});
    }

    void anUnreadableBackendTakesNothingAndKeepsTheBackup()
    {
        m_readable = false;
        m_host->takeOverStockDesktopShortcuts();
        m_readable = true;
        QCOMPARE(m_kwin.value(SwitchUp), QList<QKeySequence>{QKeySequence(QStringLiteral("Meta+Ctrl+Up"))});
        QVERIFY(!QSettings(backupPath(), QSettings::IniFormat).contains(SwitchUp));

        m_host->takeOverStockDesktopShortcuts();
        m_readable = false;
        m_host->restoreStockDesktopShortcuts();
        QVERIFY(m_kwin.value(SwitchUp).isEmpty());
        QVERIFY2(QSettings(backupPath(), QSettings::IniFormat).contains(SwitchUp),
                 "a restore that cannot reach KWin keeps every entry for the next pass");
    }

private:
    static QString backupPath()
    {
        return QStandardPaths::writableLocation(QStandardPaths::StateLocation)
            + QStringLiteral("/kwin-shortcut-backup.ini");
    }

    std::unique_ptr<IsolatedConfigGuard> m_guard;
    std::unique_ptr<QTemporaryDir> m_stateDir;
    QHash<QString, QList<QKeySequence>> m_kwin;
    bool m_readable = true;
    std::unique_ptr<Settings> m_settings;
    std::unique_ptr<ShortcutManager> m_manager;
    std::unique_ptr<KWinWorkspaceHost> m_host;
};

QTEST_MAIN(TestKWinWorkspaceHost)
#include "test_kwin_workspace_host.moc"
