// SPDX-FileCopyrightText: 2026 fuddlesworth
// SPDX-License-Identifier: GPL-3.0-or-later

// ═══════════════════════════════════════════════════════════════════════════════
// Daemon — dynamic per-monitor workspaces wiring
//
// Constructs the WorkspaceController behind the feature gate (setting +
// KWin per-output mode) and wires its verbs to the daemon. The controller owns
// the model and the census; this file owns only what needs daemon members.
// Desktops the reconciler creates or removes reach the engines, the rules and
// the placement store through the same desktopsRenumbered path as any other
// change to KWin's desktop list (desktop_renumber.cpp), so nothing here
// renumbers them a second time. A build without KDE Frameworks leaves the
// feature out entirely: it cannot read or write kwinrc the way KWin does.
// ═══════════════════════════════════════════════════════════════════════════════

#include "daemon/daemon.h"

#include "common/kwinperoutputdesktops.h"
#include "config/configdefaults.h"
#include "config/settings.h"
#include "config/settingsschema.h"
#include "core/platform/logging.h"
#include "daemon/controllers/shortcutmanager.h"
#include "daemon/controllers/shortcutmanager_ids.h"
#include "daemon/controllers/workspacecontroller.h"
#include "daemon/daemon/helpers.h"
#include "daemon/overlayservice.h"
#include "dbus/windowtrackingadaptor/lifecyclerelay.h"
#include "dbus/windowtrackingadaptor/windowtrackingadaptor.h"

#include "phosphor_i18n.h"

#include <PhosphorEngine/PlacementEngineBase.h>
#include <PhosphorIdentity/VirtualScreenId.h>
#include <PhosphorPlacement/WindowTrackingService.h>
#include <PhosphorScrollEngine/ScrollEngine.h>
#include <PhosphorScreens/ScreenIdentity.h>
#include <PhosphorWorkspaces/VirtualDesktopManager.h>
#include <PhosphorWorkspaces/WorkspaceReconciler.h>
#include <PhosphorZones/AssignmentEntry.h>

#include <QDBusConnection>
#include <QDBusMessage>
#include <QDir>
#include <QFile>
#include <QFileInfo>
#include <QHash>
#include <QKeySequence>
#include <QSettings>
#include <QStandardPaths>
#include <QUuid>

#include <memory>
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

/// Give KWin its stolen desktop-switch chords back. Runs on every stop
/// (logout included) and on every start that leaves the feature off or
/// dormant, so a takeover persisted by an earlier session is always undone.
/// Consumes the backup entry by entry, and only entries that are settled:
/// restored, or found re-bound by hand. Dropping an entry after a failed
/// call (kglobalaccel unreachable) would strand the user's chord on "none"
/// forever, while keeping it means the next restore tries again.
void restoreKWinShortcutBackup(ShortcutManager* shortcutManager)
{
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

/// The feature is off or dormant: its shortcut family holds no grabs, and
/// KWin gets back any chords a previous run took.
void parkWorkspaceShortcuts(ShortcutManager* shortcutManager)
{
    if (!shortcutManager) {
        return;
    }
    shortcutManager->setWorkspaceFamilyActive(false);
    restoreKWinShortcutBackup(shortcutManager);
}

#ifdef USE_KDE_FRAMEWORKS
void askKWinToReconfigure()
{
    QDBusConnection::sessionBus().asyncCall(
        QDBusMessage::createMethodCall(QStringLiteral("org.kde.KWin"), QStringLiteral("/KWin"),
                                       QStringLiteral("org.kde.KWin"), QStringLiteral("reconfigure")));
}

/// QKeySequence(QString) answers an empty sequence on anything it cannot
/// parse, so a typo in a named workspace's chord would bind nothing and say
/// nothing. The caller leaves an empty answer unbound. The warning latches
/// per (shortcut id, spelling), because every Named Entries change re-parses
/// every chord.
QKeySequence parseNamedChord(const QString& raw, const QString& shortcutId)
{
    if (raw.isEmpty()) {
        return {};
    }
    const QKeySequence sequence(raw);
    if (sequence.isEmpty()) {
        static QHash<QString, QString> warnedSpellings;
        if (warnedSpellings.value(shortcutId) != raw) {
            warnedSpellings.insert(shortcutId, raw);
            qCWarning(lcDaemon) << "unparseable shortcut" << raw << "for" << shortcutId << "- leaving it unbound";
        }
    }
    return sequence;
}

/// Two named-entry ids name the same entry (QUuid compare; a non-UUID id
/// matches only itself).
bool sameEntryId(const QString& a, const QString& b)
{
    const QUuid ua = QUuid::fromString(a);
    const QUuid ub = QUuid::fromString(b);
    if (!ua.isNull() && !ub.isNull()) {
        return ua == ub;
    }
    return a == b;
}

/// Whether a Workspaces.Named entry is the one the reconciler names: by id,
/// or by name for an entry written without one.
bool isNamedEntry(const QVariantMap& entry, const QString& namedId, const QString& name)
{
    const QString id = entry.value(ConfigDefaults::namedEntryIdField()).toString();
    if (!namedId.isEmpty()) {
        return !id.isEmpty() && sameEntryId(id, namedId);
    }
    return id.isEmpty() && entry.value(ConfigDefaults::namedEntryNameField()).toString() == name;
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
#endif
} // namespace

void Daemon::initializeWorkspaces()
{
#ifndef USE_KDE_FRAMEWORKS
    if (m_settings && m_settings->workspacesEnabled()) {
        qCInfo(lcDaemon) << "dynamic workspaces need KDE Frameworks, which this build leaves out";
    }
    parkWorkspaceShortcuts(m_shortcutManager.get());
#else
    if (!m_settings) {
        return;
    }
    // Already up. The re-arm paths below carry this check on their own arm;
    // the DIRECT callers (start(), and a re-entered start after a stop) did
    // not, and a second pass here would build a second controller and a
    // second m_workspaceWiring over the first.
    if (m_workspaceController) {
        return;
    }
    // Enabling the feature (or granting consent) at runtime re-enters this
    // gate without a daemon restart.
    if (!m_workspaceRearmConnected) {
        m_workspaceRearmConnected = true;
        const auto rearm = [this]() {
            // These connections outlive stop() (they hang off m_settings, not
            // m_workspaceWiring, so the runtime-disable teardown cannot reach
            // them and must not: they are what re-enables the feature). A
            // settings save on a STOPPED daemon would otherwise re-run the
            // whole bring-up on torn-down dependencies.
            if (!m_running || m_shuttingDown) {
                return;
            }
            const bool wantOn = m_settings->workspacesEnabled();
            if (wantOn && !m_workspaceController) {
                initializeWorkspaces();
            } else if (!wantOn && m_workspaceController) {
                teardownWorkspaces();
                qCInfo(lcDaemon) << "dynamic workspaces disabled at runtime";
            }
        };
        connect(m_settings.get(), &Settings::workspacesEnabledChanged, this, rearm);
        connect(m_settings.get(), &Settings::workspacesManageKWinPerOutputChanged, this, rearm);
    }
    // The compositor's own answer is the authoritative half of the gate. The
    // effect reports the running mode at bring-up and on every change: on
    // means a dormant feature can start (the user turned per-output desktops
    // on in KWin), off means a running one goes dormant (the user turned it
    // off, which is theirs to decide: kwinrc is never written back). Hung off
    // a context object parented to the adaptor's relay, so the connection is
    // made once per adaptor and goes away with it on a stop.
    if (m_windowTrackingAdaptor) {
        WindowLifecycleRelay* const relay = m_windowTrackingAdaptor->lifecycleRelay();
        const QString gateName = QStringLiteral("plasmazones-workspaces-mode-gate");
        if (relay && !relay->findChild<QObject*>(gateName, Qt::FindDirectChildrenOnly)) {
            auto* gate = new QObject(relay);
            gate->setObjectName(gateName);
            connect(relay, &WindowLifecycleRelay::perOutputDesktopsModeReported, gate, [this](bool enabled) {
                if (!m_running || m_shuttingDown || !m_settings) {
                    return;
                }
                if (enabled && !m_workspaceController && m_settings->workspacesEnabled()) {
                    initializeWorkspaces();
                } else if (!enabled && m_workspaceController) {
                    qCInfo(lcDaemon) << "KWin stopped switching desktops per output; dynamic workspaces go dormant";
                    teardownWorkspaces();
                }
            });
        }
    }
    if (!m_settings->workspacesEnabled()) {
        parkWorkspaceShortcuts(m_shortcutManager.get());
        return;
    }
    // Consent is one-shot: it authorizes ONE write of the kwinrc flag. After
    // that, per-output desktops being off again is the user's own choice, and
    // the feature stays dormant (the settings page offers the consent again)
    // rather than writing the flag back.
    bool wroteKWinConfig = false;
    if (!kwinPerOutputVirtualDesktopsEnabled()) {
        if (!m_settings->workspacesManageKWinPerOutput()) {
            qCWarning(lcDaemon) << "dynamic workspaces enabled but KWin per-output virtual desktops is off and no "
                                   "consent to manage it; feature stays dormant";
            parkWorkspaceShortcuts(m_shortcutManager.get());
            return;
        }
        if (!enableKWinPerOutputVirtualDesktops()) {
            qCWarning(lcDaemon) << "could not turn on KWin per-output virtual desktops (the kwinrc entry is locked or "
                                   "kwinrc cannot be written); dynamic workspaces stay dormant";
            parkWorkspaceShortcuts(m_shortcutManager.get());
            return;
        }
        askKWinToReconfigure();
        wroteKWinConfig = true;
        qCInfo(lcDaemon)
            << "wrote PerOutputVirtualDesktops=true to kwinrc and asked KWin to reconfigure (user consent)";
    } else if (m_windowTrackingAdaptor
               && m_windowTrackingAdaptor->perOutputDesktopsMode() == std::optional<bool>(false)) {
        // kwinrc says on, the running compositor says off: KWin has not
        // applied the file. The mode gate above starts the feature once it
        // does. Not checked right after our own write, whose stored report
        // necessarily predates the reconfigure just requested.
        qCWarning(lcDaemon) << "kwinrc enables per-output virtual desktops but KWin reports them off; dynamic "
                               "workspaces stay dormant until KWin applies the setting";
        parkWorkspaceShortcuts(m_shortcutManager.get());
        return;
    }

    m_workspaceController = std::make_unique<WorkspaceController>(m_virtualDesktopManager.get(), m_windowRegistry.get(),
                                                                  m_screenManager.get());
    // Every connection below hangs off this context object so a runtime
    // disable can sever them all at once. The daemon (`this`) stays captured
    // in the lambdas; it outlives the wiring.
    m_workspaceWiring = std::make_unique<QObject>();
    QObject* const wiring = m_workspaceWiring.get();
    WorkspaceController* const controller = m_workspaceController.get();
    PhosphorWorkspaces::WorkspaceReconciler* const reconciler = &controller->reconciler();

    // Workspace hints: the shared navigation-OSD gate first (OSD style None,
    // global suppression, the per-context rule), then the OSD style. The
    // per-context rule is keyed by the EFFECTIVE screen, so a physical id
    // from the map resolves to the virtual screen it shows on a subdivided
    // monitor. A refusal wears the cancel glyph; good news wears the
    // information glyph.
    const auto showWorkspaceHint = [this](const QString& text, const QString& screenId, bool isRefusal) {
        const QString effective = screenId.isEmpty() || PhosphorIdentity::VirtualScreenId::isVirtual(screenId)
            ? screenId
            : resolveVirtualScreenId(m_screenManager.get(), screenId, m_windowTrackingAdaptor);
        if (!navigationOsdAllowed(effective)) {
            return;
        }
        if (m_settings->osdStyle() == OsdStyle::Text) {
            showKdeTextOsd(isRefusal ? QStringLiteral("dialog-cancel") : QStringLiteral("dialog-information"), text);
            return;
        }
        if (!m_overlayService) {
            return;
        }
        if (isRefusal) {
            m_overlayService->showDisabledOsd(text, effective);
        } else {
            m_overlayService->showInfoOsd(text, effective);
        }
    };

    // Every per-output report, changed or not, reaches the controller ahead of
    // VirtualDesktopManager: the effect answers a switch it did not make with
    // a report VirtualDesktopManager swallows as unchanged, and the ledger
    // entry waiting on that answer would otherwise hold the screen until it
    // expired.
    if (m_windowTrackingAdaptor && m_windowTrackingAdaptor->lifecycleRelay()) {
        connect(m_windowTrackingAdaptor->lifecycleRelay(), &WindowLifecycleRelay::screenDesktopReported, wiring,
                [this](const QString& screenId, int desktop) {
                    if (m_workspaceController) {
                        m_workspaceController->noteScreenDesktopReport(screenId, desktop);
                    }
                });
    }

    // ── Verb execution channels ────────────────────────────────────────────
    // Per-screen switch by desktop id: reconciler-ledgered SetCurrent →
    // effect command; the answering desktopChanged report retires the ledger
    // entry.
    connect(controller, &WorkspaceController::screenDesktopSwitchRequested, wiring,
            [this](const QString& screenId, const QString& desktopId,
                   PhosphorProtocol::ScreenDesktopSwitchPurpose purpose) {
                if (m_windowTrackingAdaptor) {
                    Q_EMIT m_windowTrackingAdaptor->setScreenDesktopRequested(screenId, desktopId,
                                                                              static_cast<int>(purpose));
                }
            });
    // Window relocation: the same handoff machinery the cross-mode
    // directional moves use, same-engine allowed (plan §4.2 reuse).
    connect(controller, &WorkspaceController::windowWorkspaceMoveRequested, wiring,
            [this](const QString& windowId, const QString& targetScreenId, int targetDesktop,
                   const QString& targetDesktopId, const QString& direction, bool moveOutput, bool subjectOfVerb) {
                if (m_windowTrackingAdaptor) {
                    m_windowTrackingAdaptor->moveWindowToWorkspaceVerb(
                        windowId, targetScreenId, targetDesktop, targetDesktopId, direction, moveOutput, subjectOfVerb);
                }
            });
    // Owner-wins snap-back hint (toggleable). The user's switch was undone,
    // so it reads as a refusal.
    connect(controller, &WorkspaceController::snapBackOccurred, wiring,
            [this, showWorkspaceHint](const QString& screenId) {
                if (m_settings->workspacesSnapBackOsdHint()) {
                    showWorkspaceHint(PhosphorI18n::tr("That workspace is on another monitor.", "OSD hint"), screenId,
                                      /*isRefusal=*/true);
                }
            });
    // A window mapped onto a workspace during its removal window and KWin
    // swept it to an arbitrary neighbour; the controller re-issued a move to
    // the owner's current workspace (plan §4.3 destroy step 4).
    connect(controller, &WorkspaceController::windowDisplacedByRemoval, wiring,
            [this, showWorkspaceHint](const QString& screenId) {
                if (m_settings->workspacesSnapBackOsdHint()) {
                    showWorkspaceHint(
                        PhosphorI18n::tr("That workspace closed. The window moved to the current one.", "OSD hint"),
                        screenId, /*isRefusal=*/false);
                }
            });
    // Desktop-cap degradation hint (once per episode, reconciler-gated).
    // Screen-less: the cap is global.
    connect(reconciler, &PhosphorWorkspaces::WorkspaceReconciler::capReached, wiring, [showWorkspaceHint]() {
        showWorkspaceHint(PhosphorI18n::tr("Workspace limit reached.", "OSD hint"), QString(), /*isRefusal=*/true);
    });
    // The screen that adopts externally created desktops and realizes
    // unpinned named workspaces: the one the user is working on, asked at the
    // moment a desktop needs a home. Physical, in the effect-reported id space
    // the map keys by. Cleared in teardownWorkspaces.
    reconciler->setFocusedScreenProvider([this]() {
        const QString screenId = resolveShortcutScreenId(m_screenManager.get(), m_windowTrackingAdaptor);
        return PhosphorScreens::ScreenIdentity::idForName(
            PhosphorIdentity::VirtualScreenId::extractPhysicalId(screenId));
    });

    // ── Shortcut verbs ─────────────────────────────────────────────────────
    // The map and the per-output desktops key PHYSICAL ids. Verbs that act on
    // a window resolve the focused window's screen; verbs that act on a
    // screen resolve the screen the cursor is on, the one the user is looking
    // at.
    const auto windowScreen = [this]() {
        return PhosphorIdentity::VirtualScreenId::extractPhysicalId(
            resolveShortcutScreenId(m_screenManager.get(), m_windowTrackingAdaptor));
    };
    const auto cursorScreen = [this]() {
        return PhosphorIdentity::VirtualScreenId::extractPhysicalId(
            resolveCursorScreenId(m_screenManager.get(), m_windowTrackingAdaptor));
    };
    connect(m_shortcutManager.get(), &ShortcutManager::workspaceFocusRequested, wiring,
            [this, cursorScreen](int delta) {
                if (m_workspaceController) {
                    m_workspaceController->focusWorkspace(cursorScreen(), delta);
                }
            });
    connect(m_shortcutManager.get(), &ShortcutManager::workspaceMoveWindowRequested, wiring,
            [this, windowScreen](int delta) {
                if (!m_workspaceController || !m_windowTrackingAdaptor) {
                    return;
                }
                m_workspaceController->moveWindowToWorkspace(windowScreen(),
                                                             m_windowTrackingAdaptor->lastActiveWindowId(), delta);
            });
    connect(m_shortcutManager.get(), &ShortcutManager::workspaceMoveColumnRequested, wiring,
            [this, showWorkspaceHint](int delta) {
                if (!m_workspaceController) {
                    return;
                }
                // The column lives on a STRIP, which on a subdivided monitor
                // is a "<phys>/vs:N" virtual screen. The strip id is asked for
                // exactly; the controller's verb needs the physical one.
                const QString stripScreen = resolveShortcutScreenId(m_screenManager.get(), m_windowTrackingAdaptor);
                using Refusal = PhosphorScrollEngine::ScrollEngine::FocusedColumnRefusal;
                Refusal reason = Refusal::NotScrolling;
                auto* scroll = qobject_cast<PhosphorScrollEngine::ScrollEngine*>(m_scrollEngine.get());
                const QStringList column = scroll ? scroll->focusedColumnWindows(stripScreen, &reason) : QStringList();
                if (column.isEmpty()) {
                    QString text;
                    switch (reason) {
                    case Refusal::NotScrolling:
                        text = PhosphorI18n::tr("Moving a column needs a scrolling screen.", "OSD hint");
                        break;
                    case Refusal::FloatFocused:
                        text =
                            PhosphorI18n::tr("A floating window has focus, so there is no column to move.", "OSD hint");
                        break;
                    case Refusal::NoColumn:
                    case Refusal::None:
                        text = PhosphorI18n::tr("There is no column to move.", "OSD hint");
                        break;
                    }
                    showWorkspaceHint(text, stripScreen, /*isRefusal=*/true);
                    return;
                }
                // The column is enumerated HERE, at chord time, while the move
                // itself defers behind the reconciler's ledger: the user
                // pressed the chord on the column they were looking at, so
                // that membership is the one that travels.
                m_workspaceController->moveColumnToWorkspace(
                    PhosphorIdentity::VirtualScreenId::extractPhysicalId(stripScreen), stripScreen, column, delta);
            });
    connect(m_shortcutManager.get(), &ShortcutManager::workspaceReorderRequested, wiring,
            [this, cursorScreen](int delta) {
                if (m_workspaceController) {
                    m_workspaceController->moveWorkspace(cursorScreen(), delta);
                }
            });
    connect(m_shortcutManager.get(), &ShortcutManager::workspaceMoveToMonitorRequested, wiring,
            [this, cursorScreen](NavigationDirection direction) {
                if (m_workspaceController) {
                    m_workspaceController->moveWorkspaceToOutput(cursorScreen(), direction);
                }
            });
    connect(m_shortcutManager.get(), &ShortcutManager::workspaceMoveSlotRequested, wiring, [this](int slot) {
        // Quick-layout model: the slot's chord is fixed; what it moves the
        // window TO is the named workspace assigned in the settings app, by
        // the entry's id, with the name it had then as the fallback for an
        // entry since re-created. An unassigned slot does nothing.
        if (!m_workspaceController || !m_windowTrackingAdaptor) {
            return;
        }
        const QString targetId = m_settings->workspaceSlotTargetId(slot - 1);
        const QString targetName = m_settings->workspaceSlotTargetName(slot - 1);
        if (targetId.isEmpty() && targetName.isEmpty()) {
            return;
        }
        m_workspaceController->moveWindowToNamedWorkspace(targetId, targetName,
                                                          m_windowTrackingAdaptor->lastActiveWindowId());
    });
    connect(m_shortcutManager.get(), &ShortcutManager::workspaceFocusSlotRequested, wiring,
            [this, cursorScreen](int slot) {
                if (m_workspaceController) {
                    m_workspaceController->focusWorkspaceAt(cursorScreen(), slot - 1);
                }
            });

    // ── Named workspaces ───────────────────────────────────────────────────
    // Declarations flow from config; per-entry focus/move chords bind as
    // adhoc shortcuts keyed by the entry's persistent id (its name for an
    // entry written without one), so a rename keeps the same shortcut.
    // Only what changed is touched: an unchanged chord keeps its registration
    // and with it its place in kglobalacceld's collision order, which a
    // fresh registration would lose to every older chord on the same keys.
    // The ids the last run left behind are purged by ShortcutManager's own
    // registration pass. m_workspaceNamedShortcutIds mirrors the bound set
    // for the teardown.
    using Binding = PhosphorShortcutsIntegration::IAdhocRegistrar::AdhocBinding;
    const auto boundNamed = std::make_shared<QHash<QString, QPair<QKeySequence, QString>>>();
    const auto rebindNamedShortcuts = [this, boundNamed]() {
        QHash<QString, QPair<QKeySequence, QString>> next;
        QVector<Binding> changed;
        const auto offer = [&](const QString& id, const QString& chord, const QString& description,
                               std::function<void()> callback) {
            const QKeySequence sequence = parseNamedChord(chord, id);
            if (sequence.isEmpty()) {
                return;
            }
            const QPair<QKeySequence, QString> binding{sequence, description};
            next.insert(id, binding);
            const auto bound = boundNamed->constFind(id);
            if (bound == boundNamed->constEnd() || bound.value() != binding) {
                changed.append(Binding{id, sequence, description, std::move(callback)});
            }
        };
        const QVariantList entries = m_settings->workspacesNamedEntries();
        for (const QVariant& value : entries) {
            const QVariantMap entry = value.toMap();
            const QString entryId = entry.value(ConfigDefaults::namedEntryIdField()).toString();
            const QString name = entry.value(ConfigDefaults::namedEntryNameField()).toString();
            const QString key = entryId.isEmpty() ? name : entryId;
            offer(ShortcutIds::workspaceNamedFocusId(key),
                  entry.value(ConfigDefaults::namedEntryFocusShortcutField()).toString(),
                  PhosphorI18n::tr("Focus Workspace \"%1\"", "named workspace shortcut").arg(name),
                  [this, entryId, name]() {
                      if (m_workspaceController) {
                          m_workspaceController->focusNamedWorkspace(entryId, name);
                      }
                  });
            offer(ShortcutIds::workspaceNamedMoveId(key),
                  entry.value(ConfigDefaults::namedEntryMoveShortcutField()).toString(),
                  PhosphorI18n::tr("Move Window to Workspace \"%1\"", "named workspace shortcut").arg(name),
                  [this, entryId, name]() {
                      if (m_workspaceController && m_windowTrackingAdaptor) {
                          m_workspaceController->moveWindowToNamedWorkspace(
                              entryId, name, m_windowTrackingAdaptor->lastActiveWindowId());
                      }
                  });
        }
        QStringList released;
        for (auto it = boundNamed->constBegin(); it != boundNamed->constEnd(); ++it) {
            if (!next.contains(it.key())) {
                released.append(it.key());
            }
        }
        if (!released.isEmpty()) {
            m_shortcutManager->unregisterAdhocShortcuts(released);
        }
        if (!changed.isEmpty()) {
            m_shortcutManager->registerAdhocShortcuts(changed);
        }
        *boundNamed = next;
        m_workspaceNamedShortcutIds = next.keys();
    };
    controller->applyNamedDeclarations(m_settings->workspacesNamedEntries());
    rebindNamedShortcuts();
    connect(m_settings.get(), &Settings::workspacesNamedEntriesChanged, wiring, [this, rebindNamedShortcuts]() {
        if (m_workspaceController) {
            m_workspaceController->applyNamedDeclarations(m_settings->workspacesNamedEntries());
        }
        rebindNamedShortcuts();
    });
    // A move or reorder verb moved a named workspace. Its declaration is
    // rewritten to where it now is, or the next apply would move it back:
    // the position only where the declaration fixes one, the output only
    // where it pins one. The entry's id is carried through untouched.
    connect(reconciler, &PhosphorWorkspaces::WorkspaceReconciler::namedWorkspaceMoved, wiring,
            [this](const QString& namedId, const QString& name, const QString& screenId, int sliceIndex) {
                QVariantList entries = m_settings->workspacesNamedEntries();
                bool changed = false;
                for (QVariant& value : entries) {
                    QVariantMap entry = value.toMap();
                    if (!isNamedEntry(entry, namedId, name)) {
                        continue;
                    }
                    const int position = entry.value(ConfigDefaults::namedEntryPositionField(), -1).toInt();
                    if (position >= 0 && position != sliceIndex) {
                        entry.insert(ConfigDefaults::namedEntryPositionField(), sliceIndex);
                        changed = true;
                    }
                    const QString output = entry.value(ConfigDefaults::namedEntryOutputField()).toString();
                    const QString pinned = output.isEmpty()
                        ? QString()
                        : PhosphorScreens::ScreenIdentity::idForName(
                              PhosphorIdentity::VirtualScreenId::extractPhysicalId(output));
                    if (!pinned.isEmpty() && pinned != screenId) {
                        entry.insert(ConfigDefaults::namedEntryOutputField(), screenId);
                        changed = true;
                    }
                    value = entry;
                    break;
                }
                if (changed) {
                    m_settings->setWorkspacesNamedEntries(entries);
                    m_settings->save();
                }
            });
    // A named workspace's desktop was renamed on KWin's side (the Pager,
    // System Settings). The declaration adopts the name in place, same id,
    // unless it is one a workspace cannot carry (the shared name predicate)
    // or one another entry already has, in which case KWin gets the declared
    // name back.
    connect(reconciler, &PhosphorWorkspaces::WorkspaceReconciler::namedWorkspaceRenamedInKWin, wiring,
            [this](const QString& namedId, const QString& desktopId, const QString& kwinName) {
                if (!m_workspaceController) {
                    return;
                }
                const QString mapName = m_workspaceController->reconciler().map().entryFor(desktopId).name;
                QVariantList entries = m_settings->workspacesNamedEntries();
                int index = -1;
                for (int i = 0; i < entries.size(); ++i) {
                    if (isNamedEntry(entries.at(i).toMap(), namedId, mapName)) {
                        index = i;
                        break;
                    }
                }
                if (index < 0) {
                    return;
                }
                QVariantMap entry = entries.at(index).toMap();
                const QString declaredName = entry.value(ConfigDefaults::namedEntryNameField()).toString();
                const WorkspaceNameCheck check = checkWorkspaceName(kwinName);
                bool usable = check.verdict == WorkspaceNameVerdict::Accepted;
                for (int i = 0; usable && i < entries.size(); ++i) {
                    if (i != index
                        && entries.at(i).toMap().value(ConfigDefaults::namedEntryNameField()).toString()
                            == check.name) {
                        usable = false;
                    }
                }
                if (!usable) {
                    qCInfo(lcDaemon) << "KWin renamed workspace" << declaredName << "to" << kwinName
                                     << "which a workspace cannot be called here; restoring the declared name";
                    m_virtualDesktopManager->setDesktopName(desktopId, declaredName);
                    return;
                }
                if (check.name == declaredName) {
                    return;
                }
                entry.insert(ConfigDefaults::namedEntryNameField(), check.name);
                entries[index] = entry;
                m_settings->setWorkspacesNamedEntries(entries);
                m_settings->save();
            });

    // ── Stock KWin desktop-shortcut takeover (Phase 5) ─────────────────────
    // The stock desktop-switch actions (the "Switch One Desktop" quad plus
    // the two Walk Through Desktops chords) iterate the GLOBAL pool, tripping
    // owner-wins snap-back on nearly every press, and the quad's Down/Up
    // chords are the focus verbs' defaults. With the rebind toggle on, back
    // each chord up (state dir, once) and clear it via the KGlobalAccel
    // foreign-rebind pass-through; restore on toggle-off and on every stop.
    // If the backend cannot rebind (portal), nothing is stolen and
    // snap-back-with-hint stays the story.
    const auto applyStockRebind = [this]() {
        if (!m_settings->workspacesRebindKWinShortcuts()) {
            restoreKWinShortcutBackup(m_shortcutManager.get());
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
    };
    applyStockRebind();
    connect(m_settings.get(), &Settings::workspacesRebindKWinShortcutsChanged, wiring, applyStockRebind);

    // RouteToWorkspace rule resolver: the rules pipeline calls this on the
    // open path (rules_placement.cpp). > 0 is the REALIZED desktop the move
    // was issued to; 0 means no declaration carries the target and the
    // positional route applies; < 0 means the target IS declared but cannot
    // be placed right now, and the positional route must NOT stand in.
    if (m_windowTrackingAdaptor) {
        m_windowTrackingAdaptor->setWorkspaceRouteResolver([this](const QString& entryId, const QString& name,
                                                                  const QString& windowId, bool moveOutput,
                                                                  QString* ownerScreenOut) {
            if (!m_workspaceController) {
                return static_cast<int>(WorkspaceController::WorkspaceRouteVerdict::Unrealized);
            }
            return m_workspaceController->routeWindowToNamedWorkspace(entryId, name, windowId, moveOutput,
                                                                      ownerScreenOut);
        });
    }

    // Window → screen resolver for the owner-wins reunion arm. An engine's
    // genuine hold wins (membership-grade, unlike a raw reverse-map key a
    // refused adoption can leave behind); then an engine's tracking; then the
    // placement store's last managed-context screen for untracked (floating)
    // windows that still carry a record, looked up under the canonical id the
    // store keys by. Empty means "cannot vouch" and the controller skips the
    // check.
    controller->setWindowScreenResolver([this](const QString& windowId) -> QString {
        const PhosphorEngine::PlacementEngineBase* const engines[] = {m_scrollEngine.get(), m_autotileEngine.get(),
                                                                      m_snapEngine.get()};
        for (const auto* engine : engines) {
            if (engine) {
                const QString held = engine->heldScreenForWindow(windowId);
                if (!held.isEmpty()) {
                    return held;
                }
            }
        }
        for (const auto* engine : engines) {
            if (engine && engine->isWindowTracked(windowId)) {
                const QString tracked = engine->screenForTrackedWindow(windowId);
                if (!tracked.isEmpty()) {
                    return tracked;
                }
            }
        }
        if (m_windowTrackingAdaptor) {
            if (auto* service = m_windowTrackingAdaptor->service()) {
                if (const auto record = service->placementStore().peekExact(service->canonicalizeForLookup(windowId))) {
                    return record->screenId;
                }
            }
        }
        return QString();
    });

    // Sticky predicate for the move verbs: the SAME answer the adaptor's move
    // slot refuses on (crossmode.cpp asks WindowTrackingService::isWindowSticky
    // after shadowWindowId, which is that service's own canonicalizeForLookup,
    // so the single call here lands on the identical key). Read through the
    // daemon on every call so a torn-down service is never dereferenced; no
    // service means "cannot tell" and the verbs issue the move.
    controller->setWindowStickyPredicate([this](const QString& windowId) {
        if (!m_windowTrackingAdaptor) {
            return false;
        }
        auto* service = m_windowTrackingAdaptor->service();
        return service && service->isWindowSticky(windowId);
    });

    // The column-preserving half of move-column-to-workspace: only when the
    // destination context (that strip, the target desktop, this activity)
    // runs scrolling, since only a scrolling arrival claims the record.
    controller->setColumnStashHandler(
        [this](const QString& stripScreenId, const QStringList& windowIds, int targetDesktop) {
            auto* scroll = qobject_cast<PhosphorScrollEngine::ScrollEngine*>(m_scrollEngine.get());
            if (!scroll
                || routedModeForContext(stripScreenId, targetDesktop, currentActivity())
                    != PhosphorZones::AssignmentEntry::Mode::Scrolling) {
                return false;
            }
            return scroll->stashColumnForDesktopMove(stripScreenId, windowIds, targetDesktop);
        });

    // The strip-preserving half of move-workspace-to-monitor. The adaptor
    // groups the riders by the strip they leave and the strip they land on,
    // the way its move verb will, and records only for a scrolling landing.
    controller->setStripStashHandler([this](const QStringList& windowIds, const QString& targetScreenId, int desktop) {
        if (m_windowTrackingAdaptor) {
            m_windowTrackingAdaptor->stashScrollStripsForWorkspaceMove(windowIds, targetScreenId, desktop);
        }
    });

    controller->start();
    m_shortcutManager->setWorkspaceFamilyActive(true);
    qCInfo(lcDaemon) << "dynamic workspaces active";

    // The consent was used: the next time per-output desktops are off, the
    // user is asked again instead of the flag being written back. Last, so
    // the settings signal this raises finds the controller already up.
    if (wroteKWinConfig) {
        m_settings->setWorkspacesManageKWinPerOutput(false);
        m_settings->save();
    }
#endif
}

void Daemon::teardownWorkspaces()
{
    if (!m_workspaceController) {
        // Nothing was built (feature off, dormant gate, or already torn down).
        // The family is parked and KWin's chords restored anyway: the
        // takeover survives in the state file across a restart, so a stop()
        // with the feature since disabled must still give the chords back.
        parkWorkspaceShortcuts(m_shortcutManager.get());
        return;
    }
    // The provider captures the daemon's screen manager and adaptor, so it is
    // dropped before anything it reads can go.
    m_workspaceController->reconciler().setFocusedScreenProvider({});
    // Sever EVERY connection this feature made (m_workspaceWiring is the
    // receiver context for all of them), drop the adhoc named binds, then
    // tear the controller down (its dtor writes the final state file) and
    // hand KWin its chords back. This runs on every stop, logout included.
    // The kwinrc write is deliberately NOT reverted (stated in the UI).
    m_workspaceWiring.reset();
    if (m_shortcutManager) {
        m_shortcutManager->unregisterAdhocShortcuts(m_workspaceNamedShortcutIds);
    }
    // Cleared whether or not the unregister ran: carrying these ids into the
    // next start() would hand unregisterAdhocShortcuts a list of ids a fresh
    // registry has never heard of.
    m_workspaceNamedShortcutIds.clear();
    if (m_windowTrackingAdaptor) {
        m_windowTrackingAdaptor->setWorkspaceRouteResolver(nullptr);
    }
    m_workspaceController.reset();
    parkWorkspaceShortcuts(m_shortcutManager.get());
}

} // namespace PlasmaZones
