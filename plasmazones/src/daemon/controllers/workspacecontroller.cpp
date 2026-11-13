// SPDX-FileCopyrightText: 2026 fuddlesworth
// SPDX-License-Identifier: GPL-3.0-or-later

#include "workspacecontroller.h"

#include <PhosphorEngine/WindowRegistry.h>
#include <PhosphorIdentity/VirtualScreenId.h>
#include <PhosphorScreens/Manager.h>
#include <PhosphorScreens/PhysicalScreen.h>
#include <PhosphorScreens/ScreenIdentity.h>
#include <PhosphorWorkspaces/VirtualDesktopManager.h>

#include <QDir>
#include <QFile>
#include <QFileInfo>
#include <QLoggingCategory>
#include <QSaveFile>
#include <QScopedValueRollback>
#include <QStandardPaths>

#include <algorithm>
#include <iterator>
#include <memory>

#if __has_include(<libintl.h>)
#include <libintl.h>
#define PLASMAZONES_HAVE_LIBINTL 1
#endif

Q_LOGGING_CATEGORY(lcWorkspaceCtl, "plasmazones.workspaces.controller", QtInfoMsg)

namespace PlasmaZones {

namespace {
/// Adoption grace: how long to wait for every screen's per-output desktop
/// report before adopting the screens that did report.
constexpr int AdoptionTimeoutMs = 3000;
/// Snap-back cooldown per screen: the ledger already enforces one correction
/// in flight, but nothing bounded the rate ACROSS corrections. A sustained
/// disagreement (a screen-id mismatch between the map and the reports, or an
/// external tool fighting us) would ping-pong desktop switches at report rate
/// and take plasmashell down with the churn. One correction per screen per
/// second is ample for real snap-back and harmless as a ceiling.
constexpr qint64 SnapBackCooldownMs = 1000;
/// How many reunions in a row may be issued for one window toward the same
/// (desktop, owner) without the window reaching that owner. A move the
/// compositor refuses would otherwise be re-issued once a cooldown for the
/// life of the window.
constexpr int MaxReunionAttempts = 3;
/// State-file write debounce: map churn (a create + settle + maintenance
/// run) coalesces into one atomic write.
constexpr int StateSaveDebounceMs = 1000;

/// KWin's own label for an unnamed desktop at a 1-based position, in KWin's
/// language: KWin's defaultName() looks "Desktop %1" up in its "kwin"
/// catalog. The reconciler pushes this wherever it means "unnamed" and uses
/// it to tell a generated label from a name a user chose, so it has to match
/// what KWin writes itself. Without the catalog (not installed, or no
/// gettext) the untranslated form is what KWin shows too.
QString kwinDefaultDesktopName(int oneBasedPosition)
{
    static const QString pattern = [] {
#ifdef PLASMAZONES_HAVE_LIBINTL
        bind_textdomain_codeset("kwin", "UTF-8");
        const QString translated = QString::fromUtf8(dgettext("kwin", "Desktop %1"));
        if (translated.contains(QLatin1String("%1"))) {
            return translated;
        }
#endif
        return QStringLiteral("Desktop %1");
    }();
    return pattern.arg(oneBasedPosition);
}

/// Whether @p stamps holds a stamp for @p key younger than @p cooldownMs.
bool withinCooldown(const QHash<QString, qint64>& stamps, const QString& key, qint64 now, qint64 cooldownMs)
{
    const auto it = stamps.constFind(key);
    return it != stamps.constEnd() && now - it.value() < cooldownMs;
}
}

QString WorkspaceController::canonicalScreenId(const QString& connectorOrId)
{
    // ScreenManager hands out CONNECTOR names ("DP-2") and the effect reports
    // the EDID-style "Vendor:Model:Serial" form. Mixing the two made every
    // owner check fail, which turned every ordinary desktop switch into a
    // "foreign" one and looped snap-back against the user. idForName maps a
    // live connector to its identifier and hands anything else back as is.
    return PhosphorScreens::ScreenIdentity::idForName(connectorOrId);
}

QString WorkspaceController::screenIdOf(const PhosphorScreens::PhysicalScreen& screen)
{
    return screen.identifier.isEmpty() ? canonicalScreenId(screen.name) : screen.identifier;
}

WorkspaceController::WorkspaceController(PhosphorWorkspaces::VirtualDesktopManager* vdm,
                                         PhosphorEngine::WindowRegistry* registry,
                                         PhosphorScreens::ScreenManager* screens, QObject* parent)
    : QObject(parent)
    , m_vdm(vdm)
    , m_registry(registry)
    , m_screens(screens)
{
    m_clock.start();
    m_reconciler.setDefaultDesktopNameProvider(&kwinDefaultDesktopName);

    // Saves are scheduled on every map change but refused until adoption
    // (scheduleStateSave), so the candidate restored from last session's file
    // is never written back over that file before it has been reconciled.
    m_stateSaveTimer.setSingleShot(true);
    m_stateSaveTimer.setInterval(StateSaveDebounceMs);
    connect(&m_stateSaveTimer, &QTimer::timeout, this, &WorkspaceController::saveStateFile);
    m_adoptionTimer.setSingleShot(true);
    m_adoptionTimer.setInterval(AdoptionTimeoutMs);
    connect(&m_adoptionTimer, &QTimer::timeout, this, &WorkspaceController::adoptOnTimeout);

    wireVirtualDesktops();
    wireWindows();
    wireScreens();

    using Rec = PhosphorWorkspaces::WorkspaceReconciler;
    connect(&m_reconciler, &Rec::mapChanged, this, &WorkspaceController::scheduleStateSave);
    connect(&m_reconciler, &Rec::requestCreateDesktop, m_vdm,
            &PhosphorWorkspaces::VirtualDesktopManager::createDesktop);
    connect(&m_reconciler, &Rec::requestRemoveDesktop, m_vdm,
            &PhosphorWorkspaces::VirtualDesktopManager::removeDesktop);
    connect(&m_reconciler, &Rec::requestSetDesktopName, m_vdm,
            &PhosphorWorkspaces::VirtualDesktopManager::setDesktopName);
    connect(&m_reconciler, &Rec::resyncRequested, this, [this]() {
        // A ledger entry expired: re-pull the authoritative desktop list.
        // String-based on purpose: refreshFromKWin is a PRIVATE slot of
        // VirtualDesktopManager, so a pointer-to-member form does not compile
        // from here. The name is covered by the invoke's own runtime warning
        // if it ever moves.
        QMetaObject::invokeMethod(m_vdm, "refreshFromKWin");
    });
    connect(&m_reconciler, &Rec::foreignSwitchDetected, this,
            [this](const QString& screenId, const QString& desktopId, const QString& ownerScreenId) {
                qCInfo(lcWorkspaceCtl) << "foreign switch:" << screenId << "showed" << desktopId << "owned by"
                                       << ownerScreenId;
                snapBackScreen(screenId);
            });
    // The reconciler's per-screen switches go to the effect by desktop id; the
    // effect resolves the id itself, so no number can go stale on the way.
    // The purpose is whatever the call that made the reconciler emit set
    // (m_switchPurpose): a focus verb marks its switch a user switch, and
    // everything the reconciler does on its own stays a correction.
    //
    // Every request is answered by the effect, exactly once, including one
    // for the desktop the output already shows: that answer is a forced
    // report of the same desktop, which VirtualDesktopManager swallows as
    // unchanged and noteScreenDesktopReport hands the reconciler instead. No
    // answer is made up here. A second answer for the same request would be
    // read as the answer to the user's next press, and retire it as refused.
    connect(&m_reconciler, &Rec::requestSetCurrent, this, [this](const QString& screenId, const QString& desktopId) {
        Q_EMIT screenDesktopSwitchRequested(screenId, desktopId, m_switchPurpose);
    });
    // Deferred verbs resume once the reconciler has no create or remove in
    // flight. Queued, because the reconciler announces this from inside the
    // settle, and the other subscribers of the same desktop-list commit (the
    // daemon's desktopsRenumbered remap above all) have to have run before a
    // verb resolves anything against the new numbering.
    connect(&m_reconciler, &Rec::structuralOpsSettled, this, &WorkspaceController::drainQuietQueue,
            Qt::QueuedConnection);
    // Destroy race (plan §4.3 step 4): snapshot the census of the doomed
    // desktop; the removal echo consumes it and re-routes each window to the
    // owner's current workspace (KWin swept them to an arbitrary neighbour).
    // Windows that land on the desktop later join the record in setCensusRow.
    connect(&m_reconciler, &Rec::removalRaceDetected, this,
            [this](const QString& desktopId, const QString& ownerScreenId) {
                DisplacedByRemoval& record = m_displacedByRemoval[desktopId];
                record.ownerScreenId = ownerScreenId;
                record.windowIds.clear();
                for (auto it = m_windowCensusDesktopIds.constBegin(); it != m_windowCensusDesktopIds.constEnd(); ++it) {
                    if (it.value().contains(desktopId)) {
                        record.windowIds.append(it.key());
                    }
                }
            });
    // The removal expired or KWin refused it: the desktop stays, and so do its
    // windows, so there is nothing left to re-route.
    connect(&m_reconciler, &Rec::removalAbandoned, this, [this](const QString& desktopId) {
        m_displacedByRemoval.remove(desktopId);
    });
}

WorkspaceController::~WorkspaceController()
{
    if (m_adopted) {
        saveStateFile();
    }
}

void WorkspaceController::wireVirtualDesktops()
{
    using VDM = PhosphorWorkspaces::VirtualDesktopManager;
    using Rec = PhosphorWorkspaces::WorkspaceReconciler;
    connect(m_vdm, &VDM::kwinDesktopCreated, &m_reconciler, &Rec::onKwinDesktopCreated);
    connect(m_vdm, &VDM::kwinDesktopRemoved, &m_reconciler, &Rec::onKwinDesktopRemoved);
    connect(m_vdm, &VDM::createDesktopAnswered, this,
            [this](uint position, const QString& name, VDM::CallAnswer answer) {
                m_reconciler.onCreateDesktopAnswered(position, name, answer == VDM::CallAnswer::Answered);
            });
    connect(m_vdm, &VDM::removeDesktopAnswered, this, [this](const QString& desktopId, VDM::CallAnswer answer) {
        m_reconciler.onRemoveDesktopAnswered(desktopId, answer == VDM::CallAnswer::Answered);
    });
    connect(m_vdm, &VDM::desktopRenamed, &m_reconciler, &Rec::onKwinDesktopRenamed);
    // Removal-race consumption. The re-route bodies go through runWhenQuiet,
    // and the reconciler keeps an echoed removal pending until the settled
    // list confirms it, so they run once that list has committed and the
    // queued drain fires: the owner's current workspace is resolved against
    // the numbering the removal produced, not the one it replaced.
    connect(m_vdm, &VDM::kwinDesktopRemoved, this, [this](const QString& desktopId) {
        const auto it = m_displacedByRemoval.constFind(desktopId);
        if (it == m_displacedByRemoval.constEnd()) {
            return;
        }
        const DisplacedByRemoval record = it.value();
        m_displacedByRemoval.erase(it);
        // The hint says "the window moved", so the FIRST body that actually
        // issues a move raises it, exactly once. Every body can still bail
        // (owner has no current desktop, window closed, sticky window).
        const auto hinted = std::make_shared<bool>(false);
        for (const QString& windowId : record.windowIds) {
            const QString ownerScreen = record.ownerScreenId;
            runWhenQuiet([this, windowId, ownerScreen, hinted]() {
                if (!m_registry->metadata(windowId)) {
                    return; // closed while the body waited
                }
                const QString target = m_reconciler.currentDesktopIdOf(ownerScreen);
                const int desktop = target.isEmpty() ? 0 : m_vdm->desktopIndexOf(target);
                if (desktop <= 0) {
                    return;
                }
                // A sticky window was never displaced (it is on every
                // workspace), so no re-route is issued and no hint is raised
                // on its account.
                if (!issueWindowMove(windowId, ownerScreen, desktop, target, QStringLiteral("down"),
                                     /*moveOutput=*/true, /*subjectOfVerb=*/false)) {
                    return;
                }
                if (!*hinted) {
                    *hinted = true;
                    Q_EMIT windowDisplacedByRemoval(ownerScreen);
                }
            });
        }
    });
    // Census rows for a desktop that no longer exists. The windows KWin moved
    // off it are counted again by the post-commit reseed, against the desktop
    // they landed on.
    connect(m_vdm, &VDM::kwinDesktopRemoved, this, [this](const QString& desktopId) {
        m_populationById.remove(desktopId);
        for (auto it = m_windowCensusDesktopIds.begin(); it != m_windowCensusDesktopIds.end();) {
            it.value().removeAll(desktopId);
            it = it.value().isEmpty() ? m_windowCensusDesktopIds.erase(it) : std::next(it);
        }
    });
    connect(m_vdm, &VDM::desktopListChanged, this, [this](const QStringList& ids) {
        scheduleSettledListPass();
        if (!m_adopted) {
            tryFirstAdoption();
            return;
        }
        m_reconciler.onDesktopListSettled(ids, m_vdm->rawDesktopNames());
        // Named declarations re-verify against the settled truth (idempotent;
        // heals a create-echo FIFO mismatch via KWin's own name list).
        if (m_namedApplied) {
            applyNamedDeclarations(m_namedEntries);
        }
    });
    connect(m_vdm, &VDM::screenDesktopChanged, this, [this](const QString& screenId, int desktop) {
        if (!m_adopted) {
            tryFirstAdoption();
        }
        m_reconciler.onScreenDesktopReport(screenId, desktop);
    });
    // A replug or a screen-id rename re-records where the output already is.
    // Not a switch: the reconciler records it without matching it against a
    // switch of ours or claiming an external desktop for the screen.
    connect(m_vdm, &VDM::screenDesktopSeeded, this, [this](const QString& screenId, int desktop) {
        if (!m_adopted) {
            tryFirstAdoption();
        }
        m_reconciler.onScreenDesktopSeeded(screenId, desktop);
    });
}

void WorkspaceController::wireWindows()
{
    using Reg = PhosphorEngine::WindowRegistry;
    connect(m_registry, &Reg::windowAppeared, this, &WorkspaceController::onWindowAppeared);
    connect(m_registry, &Reg::windowDisappeared, this, &WorkspaceController::onWindowDisappeared);
    connect(m_registry, &Reg::metadataChanged, this, &WorkspaceController::onMetadataChanged);
}

void WorkspaceController::wireScreens()
{
    using SM = PhosphorScreens::ScreenManager;
    connect(m_screens, &SM::screenAdded, this, [this](const PhosphorScreens::PhysicalScreen& screen) {
        // onScreenAdded FIRST, refreshScreenOrder second, the mirror of the
        // removal path below. ScreenManager emits screenAdded only after it has
        // synced its tracked set, so refreshing the order first would already
        // have registered the new output and onScreenAdded's own
        // "do I know this screen" guard would then early-return on every
        // replug, skipping the home migration and the slice maintenance the
        // replug exists to run. The refresh that follows re-sorts the order.
        m_reconciler.onScreenAdded(screenIdOf(screen));
        refreshScreenOrder();
        // The screen set is part of the adoption gate (every known screen
        // must have reported), so a change to it can complete adoption.
        if (!m_adopted) {
            tryFirstAdoption();
        }
    });
    connect(m_screens, &SM::screenRemoved, this, [this](const PhosphorScreens::PhysicalScreen& screen) {
        const QString screenId = screenIdOf(screen);
        m_reconciler.onScreenRemoved(screenId);
        // Per-screen stamps nothing else clears: an unplugged output's
        // cooldown would swallow the first real correction after a replug,
        // and an owed snap-back answer would outlive the screen.
        m_lastSnapBackMs.remove(screenId);
        m_snapBackAnswerOwed.remove(screenId);
        m_snapBackRetryPending.remove(screenId);
        refreshScreenOrder();
        // Unplugging the last screen that had not reported completes the
        // gate for the ones that remain.
        if (!m_adopted) {
            tryFirstAdoption();
        }
    });
    connect(m_screens, &SM::screenGeometryChanged, this, [this](const PhosphorScreens::PhysicalScreen&) {
        refreshScreenOrder();
    });
    // A same-model hotplug can flip a screen's id between its bare and
    // "/CONNECTOR"-qualified spellings with NO screenAdded / screenRemoved
    // pair. The output stayed connected, so its slice, home stamps and
    // per-screen records are re-keyed in place. The desktop manager's row
    // was already re-keyed by the daemon's own handler for this signal,
    // which is connected first and runs with the feature off too.
    connect(m_screens, &SM::screenIdentifierChanged, this, [this](const QString& oldId, const QString& newId) {
        const QString from = canonicalScreenId(oldId);
        const QString to = canonicalScreenId(newId);
        if (from.isEmpty() || to.isEmpty() || from == to) {
            return;
        }
        qCInfo(lcWorkspaceCtl) << "screen id changed from" << from << "to" << to << "- re-keying its slice";
        // The snap-back state follows the id the way the reconciler's
        // per-screen records do: a record already under the new id is the
        // fresher one and is kept. Re-keyed BEFORE the reconciler's rename,
        // which re-evaluates the screen under its new id at once: with the
        // stamp still under the old id, that evaluation would find no
        // cooldown and correct the screen again straight away.
        if (const auto stamp = m_lastSnapBackMs.constFind(from); stamp != m_lastSnapBackMs.constEnd()) {
            const qint64 value = stamp.value();
            m_lastSnapBackMs.remove(from);
            if (!m_lastSnapBackMs.contains(to)) {
                m_lastSnapBackMs.insert(to, value);
            }
        }
        if (const auto owed = m_snapBackAnswerOwed.constFind(from); owed != m_snapBackAnswerOwed.constEnd()) {
            const QString value = owed.value();
            m_snapBackAnswerOwed.remove(from);
            if (!m_snapBackAnswerOwed.contains(to)) {
                m_snapBackAnswerOwed.insert(to, value);
            }
        }
        // A retry waiting under the old id would ask snapBack for a screen the
        // map no longer knows and do nothing, leaving the output on the
        // foreign desktop. It is re-armed under the new id for what is left
        // of the cooldown; the old timer still fires and finds nothing to do.
        if (m_snapBackRetryPending.remove(from) && !m_snapBackRetryPending.contains(to)) {
            const qint64 elapsed = m_clock.elapsed() - m_lastSnapBackMs.value(to, m_clock.elapsed());
            scheduleSnapBackRetry(to, qMax<qint64>(0, SnapBackCooldownMs - elapsed));
        }
        m_reconciler.onScreenRenamed(from, to);
        refreshScreenOrder();
    });
}

void WorkspaceController::refreshScreenOrder()
{
    // Slice-concatenation order (fork 5): left-to-right by geometry, ties by
    // connector name for determinism.
    auto screens = m_screens->screens();
    std::sort(screens.begin(), screens.end(),
              [](const PhosphorScreens::PhysicalScreen& a, const PhosphorScreens::PhysicalScreen& b) {
                  if (a.geometry.x() != b.geometry.x()) {
                      return a.geometry.x() < b.geometry.x();
                  }
                  if (a.geometry.y() != b.geometry.y()) {
                      return a.geometry.y() < b.geometry.y();
                  }
                  return a.name < b.name;
              });
    QStringList order;
    order.reserve(screens.size());
    for (const auto& screen : screens) {
        order.append(screenIdOf(screen));
    }
    m_reconciler.onScreenOrderChanged(order);
}

QString WorkspaceController::stateFilePath()
{
    // Runtime state, deliberately NOT config.json and not GenericDataLocation
    // (that tree is user-visible assets). Reconstructible by definition, so a
    // version mismatch or parse failure just falls back to fresh adoption.
    // StateLocation is already app-scoped (~/.local/state/<org>/<app>).
    return QStandardPaths::writableLocation(QStandardPaths::StateLocation) + QStringLiteral("/workspaces.json");
}

void WorkspaceController::loadStateFile()
{
    QFile file(stateFilePath());
    if (!file.open(QIODevice::ReadOnly)) {
        return;
    }
    if (!m_reconciler.restoreState(QString::fromUtf8(file.readAll()))) {
        qCWarning(lcWorkspaceCtl) << "workspace state file unreadable or wrong version; re-adopting fresh";
        return;
    }
    qCInfo(lcWorkspaceCtl) << "loaded workspace state candidate:" << m_reconciler.map().allDesktopIds().size()
                           << "desktops," << m_reconciler.map().screenOrder().size() << "screens";
}

void WorkspaceController::saveStateFile() const
{
    if (!m_adopted) {
        return;
    }
    const QString path = stateFilePath();
    QDir().mkpath(QFileInfo(path).absolutePath());
    QSaveFile file(path);
    if (!file.open(QIODevice::WriteOnly)) {
        qCWarning(lcWorkspaceCtl) << "cannot write workspace state file" << path;
        return;
    }
    // Only screens whose current desktop the reconciler actually knows
    // contribute one: a guessed current would assert a workspace the screen
    // is not showing.
    const QString json = m_reconciler.map().toJson(
        m_reconciler.generation(), m_reconciler.currentIndexByScreen(),
        [this](const QString& id) {
            return m_vdm->desktopIndexOf(id);
        },
        /*includeState=*/true);
    file.write(json.toUtf8());
    if (!file.commit()) {
        // QSaveFile swallows a failed rename otherwise, and this is the one
        // write that decides whether ownership survives the restart.
        qCWarning(lcWorkspaceCtl) << "could not commit workspace state file" << path << ":" << file.errorString();
    }
}

void WorkspaceController::scheduleStateSave()
{
    if (!m_adopted) {
        return;
    }
    m_stateSaveTimer.start();
}

void WorkspaceController::start()
{
    // Candidate map from the previous session; adoption reconciles it against
    // reality (ids in both keep owner/name/home; vanished ids drop; new ids
    // adopt). Loaded BEFORE the screen order so a stored slice for a screen
    // that is gone survives long enough to be migrated with home stamping.
    // Nothing below is saved: the map is only a candidate until adoption.
    loadStateFile();
    const QStringList storedScreens = m_reconciler.map().screenOrder();

    refreshScreenOrder();

    // A stored screen that is not connected right now: its slice is fostered
    // by a surviving screen with homeScreenId stamped, exactly like a live
    // unplug, and replugging it later brings the workspaces home. The
    // reconciler only records this before adoption; maintenance, reaping and
    // owner-wins corrections all wait for adoptAll.
    QStringList liveScreens;
    const auto screens = m_screens->screens();
    liveScreens.reserve(screens.size());
    for (const auto& screen : screens) {
        liveScreens.append(screenIdOf(screen));
    }
    for (const QString& stored : storedScreens) {
        if (!liveScreens.contains(stored)) {
            m_reconciler.onScreenRemoved(stored);
        }
    }

    // Census seed from the registry's current truth (windows that registered
    // before the feature came up). Recorded only, like the fostering above.
    const QStringList ids = m_registry->instanceIds();
    for (const QString& instanceId : ids) {
        onWindowAppeared(instanceId);
    }

    tryFirstAdoption();
    if (!m_adopted) {
        m_adoptionTimer.start();
    }
}

void WorkspaceController::tryFirstAdoption()
{
    if (m_adopted) {
        return;
    }
    const QStringList ids = m_vdm->desktopIds();
    const QStringList order = m_reconciler.map().screenOrder();
    if (ids.isEmpty() || order.isEmpty()) {
        return;
    }
    // Gate on every known screen having a REAL per-output report (plan §4.3).
    // currentDesktopForScreen falls back to the global current for unknown
    // screens, which would satisfy this loop before the effect's bringup
    // re-sync (the §4.6 ordering anchor) and hand nearly everything to the
    // first screen, so ask for report presence explicitly. The reports come
    // from the effect, which re-pushes every window's metadata ahead of them
    // on the same connection, so by now the census is seeded too.
    QHash<QString, QString> currentById;
    for (const QString& screenId : order) {
        if (!m_vdm->hasScreenDesktopReport(screenId)) {
            return;
        }
        const QString id = m_vdm->desktopIdAt(m_vdm->currentDesktopForScreen(screenId));
        if (id.isEmpty()) {
            return;
        }
        currentById.insert(screenId, id);
    }
    if (!m_reconciler.adoptAll(ids, currentById, preferredScreenByDesktopId(), m_vdm->rawDesktopNames())) {
        return;
    }
    m_adopted = true;
    m_adoptionTimer.stop();
    qCInfo(lcWorkspaceCtl) << "adopted" << ids.size() << "desktops across" << order.size() << "screens";
    applyNamedDeclarationsAfterAdoption();
    scheduleStateSave();
}

void WorkspaceController::adoptOnTimeout()
{
    if (m_adopted) {
        return;
    }
    // Only screens that REPORTED contribute a current desktop. The global
    // fallback names a desktop such a screen may not be showing, and seeding
    // the map with it hands that desktop to the wrong slice.
    const QStringList ids = m_vdm->desktopIds();
    const QStringList order = m_reconciler.map().screenOrder();
    QHash<QString, QString> currentById;
    bool anyReport = false;
    for (const QString& screenId : order) {
        if (!m_vdm->hasScreenDesktopReport(screenId)) {
            continue;
        }
        anyReport = true;
        const QString id = m_vdm->desktopIdAt(m_vdm->currentDesktopForScreen(screenId));
        if (!id.isEmpty()) {
            currentById.insert(screenId, id);
        }
    }
    // No screen reported at all means the compositor side is not there (the
    // KWin effect is not loaded, or not yet). Then nothing has pushed the
    // window set either, every desktop reads empty to the census, and
    // adopting would let maintenance reap every unnamed desktop that holds
    // windows. Keep waiting; the effect's first reports adopt through
    // tryFirstAdoption, or the next timeout does.
    if (!anyReport) {
        qCInfo(lcWorkspaceCtl) << "adoption waiting: the compositor has not reported any screen yet";
        m_adoptionTimer.start();
        return;
    }
    if (!m_reconciler.adoptAll(ids, currentById, preferredScreenByDesktopId(), m_vdm->rawDesktopNames())) {
        qCWarning(lcWorkspaceCtl) << "adoption timeout: the desktop list or the screen order is empty; retrying";
        m_adoptionTimer.start();
        return;
    }
    m_adopted = true;
    qCWarning(lcWorkspaceCtl) << "adoption timeout: adopted with" << currentById.size() << "of" << order.size()
                              << "screens reporting a desktop";
    // The SAME tail tryFirstAdoption runs, or the named declarations parked
    // while adoption was pending would never be realized this session.
    applyNamedDeclarationsAfterAdoption();
    scheduleStateSave();
}

QHash<QString, QString> WorkspaceController::preferredScreenByDesktopId() const
{
    QHash<QString, QString> preferred;
    if (!m_windowScreenResolver) {
        return preferred;
    }
    const QStringList order = m_reconciler.map().screenOrder();
    // desktopId -> screen -> windows of that desktop on that screen. The
    // census already leaves out sticky windows; a window on several desktops
    // counts toward each, as it does for population.
    QHash<QString, QHash<QString, int>> tally;
    for (auto it = m_windowCensusDesktopIds.constBegin(); it != m_windowCensusDesktopIds.constEnd(); ++it) {
        const QString reported = m_windowScreenResolver(it.key());
        if (reported.isEmpty()) {
            continue;
        }
        const QString screen = canonicalScreenId(PhosphorIdentity::VirtualScreenId::extractPhysicalId(reported));
        if (!order.contains(screen)) {
            continue;
        }
        for (const QString& desktopId : it.value()) {
            ++tally[desktopId][screen];
        }
    }
    for (auto it = tally.constBegin(); it != tally.constEnd(); ++it) {
        QString best;
        int bestCount = 0;
        // Walk the screen order, not the hash, so a tie goes to the same
        // screen on every run.
        for (const QString& screen : order) {
            const int count = it.value().value(screen, 0);
            if (count > bestCount) {
                best = screen;
                bestCount = count;
            }
        }
        if (!best.isEmpty()) {
            preferred.insert(it.key(), best);
        }
    }
    return preferred;
}

void WorkspaceController::applyNamedDeclarationsAfterAdoption()
{
    // applyNamedDeclarations parks its entries and returns while !m_adopted,
    // so BOTH adoption paths owe it this call. Run with an empty list too:
    // the restored map can carry names no declaration claims any more, and
    // only an apply reverts them.
    if (!m_namedApplied) {
        applyNamedDeclarations(m_namedEntries);
    }
    // Owner-wins sweep over the seeded census. start() feeds the registry's
    // existing windows through onWindowAppeared BEFORE adoption, and
    // reuniteWindowWithOwner refuses while !m_adopted, so every window that
    // existed at bring-up would sit on a foreign output until its next
    // metadata event, which for an idle window never comes. The per-window
    // cooldown and the one-in-flight guard bound the burst.
    const QStringList seeded = m_windowCensusDesktopIds.keys();
    for (const QString& instanceId : seeded) {
        reuniteWindowWithOwner(instanceId);
    }
    drainParkedNamedRoutes();
}

void WorkspaceController::snapBackScreen(const QString& screenId)
{
    if (!m_adopted || !m_reconciler.map().knowsScreen(screenId)) {
        return;
    }
    // Owner-wins: return the screen to its own slice. Two guards: the
    // reconciler's ledger allows one correction in flight, and the cooldown
    // bounds the rate ACROSS corrections so a sustained disagreement degrades
    // to a once-a-second nudge instead of a desktop-switch storm.
    const qint64 now = m_clock.elapsed();
    if (const auto stamp = m_lastSnapBackMs.constFind(screenId);
        stamp != m_lastSnapBackMs.constEnd() && now - stamp.value() < SnapBackCooldownMs) {
        // Refused for now, not for good: nothing else re-reports the screen,
        // so without a retry it would stay on the foreign desktop. The retry
        // re-asks snapBack, which does nothing if the screen has since come
        // back to a workspace of its own.
        if (!m_snapBackRetryPending.contains(screenId)) {
            scheduleSnapBackRetry(screenId, SnapBackCooldownMs - (now - stamp.value()));
        }
        return;
    }
    const QScopedValueRollback purpose(m_switchPurpose, PhosphorProtocol::ScreenDesktopSwitchPurpose::Correction);
    if (!m_reconciler.snapBack(screenId)) {
        return;
    }
    m_lastSnapBackMs.insert(screenId, now);
    const QString target = m_reconciler.inFlightSetCurrentTarget(screenId);
    if (!target.isEmpty()) {
        m_snapBackAnswerOwed.insert(screenId, target);
    }
    Q_EMIT snapBackOccurred(screenId);
}

void WorkspaceController::scheduleSnapBackRetry(const QString& screenId, qint64 delayMs)
{
    m_snapBackRetryPending.insert(screenId);
    QTimer::singleShot(delayMs, this, [this, screenId]() {
        // Only the retry that still holds the flag runs: one superseded by a
        // screen-id rename finds its flag gone and leaves the screen to the
        // retry armed under the new id.
        if (!m_snapBackRetryPending.remove(screenId)) {
            return;
        }
        snapBackScreen(screenId);
    });
}

bool WorkspaceController::takeDesktopSwitchOsdSuppression(const QString& screenId, int desktop)
{
    if (!m_adopted) {
        return false;
    }
    const QString desktopId = m_vdm->desktopIdAt(desktop);
    if (desktopId.isEmpty()) {
        return false;
    }
    const QString owner = m_reconciler.map().ownerOf(desktopId);
    if (!owner.isEmpty() && owner != screenId) {
        // The screen landed on another screen's workspace: owner-wins takes
        // it straight back, so a card naming that workspace would announce a
        // switch the user never keeps. An owed answer stays owed: it is the
        // snap-back this very report just provoked, and its answer is still
        // to come.
        return true;
    }
    const auto owed = m_snapBackAnswerOwed.find(screenId);
    if (owed == m_snapBackAnswerOwed.end()) {
        return false;
    }
    const bool isAnswer = owed.value() == desktopId;
    m_snapBackAnswerOwed.erase(owed);
    return isAnswer;
}

void WorkspaceController::noteScreenDesktopReport(const QString& screenId, int desktop)
{
    if (desktop <= 0) {
        return;
    }
    // A snap-back's answer naming another desktop means the correction did
    // not happen (refused, deferred, or switched straight back), and it is
    // usually the desktop the output already shows, which never reaches
    // takeDesktopSwitchOsdSuppression. Left owed, the marker would silence
    // the card of a later switch the user makes to that desktop by hand.
    if (const auto owed = m_snapBackAnswerOwed.constFind(screenId);
        owed != m_snapBackAnswerOwed.constEnd() && owed.value() != m_vdm->desktopIdAt(desktop)) {
        m_snapBackAnswerOwed.erase(owed);
    }
    if (m_reconciler.inFlightSetCurrentTarget(screenId).isEmpty()) {
        return;
    }
    // A changed report is announced by VirtualDesktopManager and reaches the
    // reconciler through the screenDesktopChanged arm; feeding it here too
    // would record it twice.
    if (!m_vdm->hasScreenDesktopReport(screenId) || m_vdm->currentDesktopForScreen(screenId) != desktop) {
        return;
    }
    m_reconciler.onScreenDesktopReport(screenId, desktop);
}

void WorkspaceController::setWindowScreenResolver(std::function<QString(const QString&)> resolver)
{
    m_windowScreenResolver = std::move(resolver);
}

void WorkspaceController::setWindowStickyPredicate(std::function<bool(const QString&)> predicate)
{
    m_windowStickyPredicate = std::move(predicate);
}

void WorkspaceController::setColumnStashHandler(
    std::function<bool(const QString& stripScreenId, const QStringList& windowIds, int targetDesktop)> handler)
{
    m_columnStashHandler = std::move(handler);
}

void WorkspaceController::setStripStashHandler(
    std::function<void(const QStringList& windowIds, const QString& targetScreenId, int desktop)> handler)
{
    m_stripStashHandler = std::move(handler);
}

void WorkspaceController::reuniteWindowWithOwner(const QString& instanceId)
{
    // Owner-wins, second arm (plan §4.7): the window follows its desktop's
    // owner screen. A window whose desktop belongs to another output would
    // otherwise sit invisible (its own output may never show that desktop
    // again once snap-back holds).
    if (!m_adopted || !m_windowScreenResolver) {
        return;
    }
    // A window on several desktops can have several owners; there is no one
    // screen to move it to.
    const QStringList desktopIds = m_windowCensusDesktopIds.value(instanceId);
    if (desktopIds.size() != 1) {
        return;
    }
    const QString desktopId = desktopIds.constFirst();
    if (m_pendingWindowMoves.contains(instanceId)) {
        return; // our own move in flight; its arrival re-runs this check
    }
    const QString owner = m_reconciler.map().ownerOf(desktopId);
    if (owner.isEmpty()) {
        return;
    }
    const QString reported = m_windowScreenResolver(instanceId);
    if (reported.isEmpty()) {
        return; // screen unknown (untracked window): nothing to vouch for
    }
    const QString windowScreen = canonicalScreenId(PhosphorIdentity::VirtualScreenId::extractPhysicalId(reported));
    if (windowScreen == owner) {
        m_reunionAttempts.remove(instanceId);
        return;
    }
    if (withinCooldown(m_lastReunionMs, instanceId, m_clock.elapsed(), SnapBackCooldownMs)) {
        return;
    }
    // Convergence budget: the same reunion issued MaxReunionAttempts times
    // without the window reaching the owner is a move the compositor will not
    // perform. Left alone until the window's desktop or the owner changes.
    if (const auto attempt = m_reunionAttempts.constFind(instanceId); attempt != m_reunionAttempts.constEnd()
        && attempt->desktopId == desktopId && attempt->ownerScreenId == owner && attempt->count >= MaxReunionAttempts) {
        return;
    }
    // One reunion in flight per window. The cooldown alone could not bound the
    // QUEUE: while a structural op is pending every arriving report defers
    // another body, and they would all run in one burst when the ledger
    // quiets, each re-emitting the same move.
    if (m_pendingReunions.contains(instanceId)) {
        return;
    }
    m_pendingReunions.insert(instanceId);
    runWhenQuiet([this, instanceId, desktopId, owner]() {
        m_pendingReunions.remove(instanceId);
        if (!m_registry->metadata(instanceId)) {
            return; // closed while the body waited
        }
        if (m_reconciler.map().ownerOf(desktopId) != owner
            || !m_windowCensusDesktopIds.value(instanceId).contains(desktopId)) {
            return; // ownership or the window's desktop moved while deferred
        }
        // Re-checked HERE, not only at entry: a displacement re-route can have
        // queued its own move for this window in the meantime, and both bodies
        // drain from the same quiet queue.
        if (m_pendingWindowMoves.contains(instanceId) || isStickyWindow(instanceId)) {
            return;
        }
        const int desktop = m_vdm->desktopIndexOf(desktopId);
        if (desktop <= 0) {
            return;
        }
        ReunionAttempt& attempt = m_reunionAttempts[instanceId];
        if (attempt.desktopId != desktopId || attempt.ownerScreenId != owner) {
            attempt = ReunionAttempt{desktopId, owner, 0};
        }
        ++attempt.count;
        if (attempt.count == MaxReunionAttempts) {
            qCInfo(lcWorkspaceCtl) << "window" << instanceId << "has not reached" << owner << "after" << attempt.count
                                   << "reunions; this is the last until its workspace or its owner changes";
        }
        // Cooldown stamped at the EMIT, not at entry: a body that bailed on
        // any guard above issued nothing.
        m_lastReunionMs.insert(instanceId, m_clock.elapsed());
        qCInfo(lcWorkspaceCtl) << "reuniting window" << instanceId << "with its workspace's owner screen" << owner;
        Q_EMIT windowWorkspaceMoveRequested(instanceId, owner, desktop, desktopId, QStringLiteral("down"),
                                            /*moveOutput=*/true, /*subjectOfVerb=*/false);
    });
}

QStringList WorkspaceController::censusDesktopIds(const PhosphorEngine::WindowMetadata& meta) const
{
    if (meta.isSticky.value_or(false)) {
        return {};
    }
    // virtualDesktops lists every desktop of a window placed on several; a
    // window on one leaves it empty or names only that one.
    const QList<int> desktops = meta.virtualDesktops.isEmpty() ? QList<int>{meta.virtualDesktop} : meta.virtualDesktops;
    QStringList ids;
    for (const int desktop : desktops) {
        if (desktop <= 0) {
            continue;
        }
        const QString id = m_vdm->desktopIdAt(desktop);
        if (!id.isEmpty() && !ids.contains(id)) {
            ids.append(id);
        }
    }
    return ids;
}

void WorkspaceController::setCensusRow(const QString& instanceId, const QStringList& desktopIds)
{
    const QStringList previous = m_windowCensusDesktopIds.value(instanceId);
    if (previous == desktopIds) {
        return;
    }
    // The row is written BEFORE any population moves: a population change can
    // make the reconciler emit removalRaceDetected synchronously, and its
    // handler snapshots the census from this hash.
    if (desktopIds.isEmpty()) {
        m_windowCensusDesktopIds.remove(instanceId);
    } else {
        m_windowCensusDesktopIds.insert(instanceId, desktopIds);
    }
    for (const QString& id : previous) {
        if (!desktopIds.contains(id)) {
            adjustPopulationById(id, -1);
        }
    }
    for (const QString& id : desktopIds) {
        if (previous.contains(id)) {
            continue;
        }
        adjustPopulationById(id, +1);
        // A window arriving on a desktop whose removal already raced is swept
        // by KWin like the ones the snapshot caught.
        if (const auto record = m_displacedByRemoval.find(id); record != m_displacedByRemoval.end()) {
            if (!record->windowIds.contains(instanceId)) {
                record->windowIds.append(instanceId);
            }
        }
    }
}

void WorkspaceController::adjustPopulationById(const QString& desktopId, int delta)
{
    if (desktopId.isEmpty()) {
        return;
    }
    const int next = qMax(0, m_populationById.value(desktopId, 0) + delta);
    if (next == 0) {
        // An empty desktop and an unknown one read the same to every consumer
        // (value(id, 0)), so drop the row instead of keeping a zero around
        // for a desktop that may since have been destroyed.
        m_populationById.remove(desktopId);
    } else {
        m_populationById.insert(desktopId, next);
    }
    m_reconciler.onPopulationChanged(desktopId, next);
}

void WorkspaceController::scheduleSettledListPass()
{
    if (m_settledListPassQueued) {
        return;
    }
    m_settledListPassQueued = true;
    QMetaObject::invokeMethod(this, &WorkspaceController::runSettledListPass, Qt::QueuedConnection);
}

void WorkspaceController::runSettledListPass()
{
    m_settledListPassQueued = false;
    reseedCensus();
    // A route parked because its realized desktop had no live number yet is
    // waiting for exactly this list. Routes parked for churn or adoption are
    // re-parked by the drain until those clear.
    if (m_adopted && !m_reconciler.hasPendingStructuralOps()) {
        drainParkedNamedRoutes();
    }
}

void WorkspaceController::reseedCensus()
{
    // Event-time translation reads a window's desktop NUMBER against whichever
    // list is current when the event lands, and that goes wrong around a
    // change: a number pushed in the new numbering before the list committed
    // resolves to the wrong id or to none, and the windows KWin moves off a
    // removed desktop keep their number, so no metadata event recounts them.
    // The registry holds every window's number in the committed numbering by
    // the time this runs, so the census is rebuilt from it outright.
    QHash<QString, QStringList> rows;
    QHash<QString, int> population;
    const QStringList instances = m_registry->instanceIds();
    for (const QString& instanceId : instances) {
        const auto meta = m_registry->metadata(instanceId);
        if (!meta) {
            continue;
        }
        const QStringList ids = censusDesktopIds(*meta);
        if (ids.isEmpty()) {
            continue;
        }
        rows.insert(instanceId, ids);
        for (const QString& id : ids) {
            ++population[id];
        }
    }
    const QHash<QString, QStringList> previousRows = std::exchange(m_windowCensusDesktopIds, rows);
    const QHash<QString, int> previousPopulation = std::exchange(m_populationById, population);

    // Windows that now sit on a doomed desktop join its removal record, and a
    // move whose arrival the event-time translation missed is retired.
    for (auto it = rows.constBegin(); it != rows.constEnd(); ++it) {
        for (const QString& id : it.value()) {
            if (const auto record = m_displacedByRemoval.find(id); record != m_displacedByRemoval.end()) {
                if (!record->windowIds.contains(it.key())) {
                    record->windowIds.append(it.key());
                }
            }
        }
        if (const auto pending = m_pendingWindowMoves.constFind(it.key());
            pending != m_pendingWindowMoves.constEnd() && it.value().contains(pending.value())) {
            m_pendingWindowMoves.remove(it.key());
            m_windowMoveSequences.remove(it.key());
        }
    }

    QSet<QString> touched;
    for (auto it = previousPopulation.constBegin(); it != previousPopulation.constEnd(); ++it) {
        touched.insert(it.key());
    }
    for (auto it = population.constBegin(); it != population.constEnd(); ++it) {
        touched.insert(it.key());
    }
    for (const QString& id : std::as_const(touched)) {
        const int next = population.value(id, 0);
        if (next == previousPopulation.value(id, 0)) {
            continue;
        }
        // A desktop that left the list needs no zero: the reconciler drops
        // its own row when the desktop goes.
        if (next == 0 && m_vdm->desktopIndexOf(id) <= 0) {
            continue;
        }
        m_reconciler.onPopulationChanged(id, next);
    }
    for (auto it = rows.constBegin(); it != rows.constEnd(); ++it) {
        if (previousRows.value(it.key()) != it.value()) {
            reuniteWindowWithOwner(it.key());
        }
    }
}

void WorkspaceController::onWindowAppeared(const QString& instanceId)
{
    const auto meta = m_registry->metadata(instanceId);
    if (!meta) {
        return;
    }
    setCensusRow(instanceId, censusDesktopIds(*meta));
    reuniteWindowWithOwner(instanceId);
}

void WorkspaceController::onWindowDisappeared(const QString& instanceId)
{
    // Per-window bookkeeping outlives the census entry and has to go with it:
    // left behind, it grows without bound for the life of the daemon, and a
    // reused instance id would inherit a cooldown or a spent reunion budget.
    m_lastReunionMs.remove(instanceId);
    m_pendingReunions.remove(instanceId);
    m_reunionAttempts.remove(instanceId);
    m_pendingWindowMoves.remove(instanceId);
    m_windowMoveSequences.remove(instanceId);
    m_parkedNamedRoutes.remove(instanceId);
    // A closed window also leaves the removal-race snapshots. A record whose
    // last window just went away has nothing left to re-route.
    for (auto it = m_displacedByRemoval.begin(); it != m_displacedByRemoval.end();) {
        it.value().windowIds.removeAll(instanceId);
        it = it.value().windowIds.isEmpty() ? m_displacedByRemoval.erase(it) : std::next(it);
    }
    setCensusRow(instanceId, {});
}

void WorkspaceController::onMetadataChanged(const QString& instanceId, const PhosphorEngine::WindowMetadata& oldMeta,
                                            const PhosphorEngine::WindowMetadata& newMeta)
{
    Q_UNUSED(oldMeta)
    const QStringList newIds = censusDesktopIds(newMeta);
    // A watched move verb confirmed by its arrival (watchdog, plan §4.2): the
    // window is on the target now, alone or among the desktops of a window
    // placed on several, or it went sticky and is on every desktop.
    if (const auto pending = m_pendingWindowMoves.constFind(instanceId); pending != m_pendingWindowMoves.constEnd()
        && (newMeta.isSticky.value_or(false) || newIds.contains(pending.value()))) {
        m_pendingWindowMoves.remove(instanceId);
        m_windowMoveSequences.remove(instanceId);
    }
    setCensusRow(instanceId, newIds);
    // Re-checked on every push, not only when the desktop moved. KWin usually
    // re-desks a window dragged to another monitor onto that output's
    // current workspace, which the census change covers, but a push can also
    // carry a new output alone.
    reuniteWindowWithOwner(instanceId);
}

PhosphorWorkspaces::WorkspaceReconciler& WorkspaceController::reconciler()
{
    return m_reconciler;
}

} // namespace PlasmaZones
