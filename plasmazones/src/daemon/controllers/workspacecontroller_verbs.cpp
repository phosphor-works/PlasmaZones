// SPDX-FileCopyrightText: 2026 fuddlesworth
// SPDX-License-Identifier: GPL-3.0-or-later

// WorkspaceController — verb execution (plan §4.2), split from
// workspacecontroller.cpp by concern. Every verb resolves its target desktop
// by id inside the reconciler and translates to the live number only at emit
// time, deferring behind the ledger while a structural op is in flight so a
// renumbering window can never hand a verb a stale index.

#include "workspacecontroller.h"

#include "config/configdefaults.h"
#include "daemon/controllers/shortcutmanager.h"

#include <PhosphorEngine/WindowRegistry.h>
#include <PhosphorIdentity/VirtualScreenId.h>
#include <PhosphorIdentity/WindowId.h>
#include <PhosphorWorkspaces/VirtualDesktopManager.h>

#include <QLoggingCategory>
#include <QScopedValueRollback>
#include <QTimer>
#include <QUuid>

#include <utility>

Q_DECLARE_LOGGING_CATEGORY(lcWorkspaceCtl)

namespace PlasmaZones {

namespace {
/// How long a move verb waits for its census arrival before the watchdog
/// declares it lost (effect unloaded, window gone, refusal upstream).
constexpr int WindowMoveTimeoutMs = 2000;

/// Two named-entry ids name the same entry. Config ids are canonical braced
/// UUIDs, a rule may carry another spelling of the same UUID, and an id that
/// is not a UUID at all only matches itself.
bool sameEntryId(const QString& a, const QString& b)
{
    const QUuid ua = QUuid::fromString(a);
    const QUuid ub = QUuid::fromString(b);
    if (!ua.isNull() && !ub.isNull()) {
        return ua == ub;
    }
    return a == b;
}
}

bool WorkspaceController::isStickyWindow(const QString& windowId) const
{
    // The adaptor's move slot drops a desktop move for a window on all
    // desktops outright (crossmode.cpp), so a move for one is never issued.
    return m_windowStickyPredicate && m_windowStickyPredicate(windowId);
}

void WorkspaceController::armMoveWatch(const QString& windowId, const QString& targetDesktopId)
{
    // Sequence token per watch, not just the target id: two moves of the same
    // window to the same desktop (a repeated chord, a reunion following a
    // displacement) are indistinguishable by target, so the FIRST timer would
    // match the SECOND watch's entry, clear it and warn while the second move
    // was still in flight.
    // Keyed on the BARE instance id: the shortcut verbs pass the
    // registry-canonical COMPOSITE "appId|instanceId", while the census
    // arrival that retires a watch is keyed on the instance id the registry
    // reports. The instance id is also the one that cannot change mid-life.
    const QString watchKey = PhosphorIdentity::WindowId::extractInstanceId(windowId);
    const quint64 sequence = ++m_windowMoveSequence;
    m_pendingWindowMoves.insert(watchKey, targetDesktopId);
    m_windowMoveSequences.insert(watchKey, sequence);
    QTimer::singleShot(WindowMoveTimeoutMs, this, [this, watchKey, windowId, targetDesktopId, sequence]() {
        if (m_windowMoveSequences.value(watchKey) != sequence) {
            return; // superseded by a later watch (or already retired)
        }
        if (m_pendingWindowMoves.value(watchKey) == targetDesktopId) {
            m_pendingWindowMoves.remove(watchKey);
            m_windowMoveSequences.remove(watchKey);
            qCWarning(lcWorkspaceCtl) << "workspace move for window" << windowId << "to desktop" << targetDesktopId
                                      << "saw no arrival (effect not loaded, window closed, or handoff refused)";
        }
    });
}

bool WorkspaceController::issueWindowMove(const QString& windowId, const QString& targetScreenId, int desktop,
                                          const QString& targetDesktopId, const QString& direction, bool moveOutput,
                                          bool subjectOfVerb)
{
    if (isStickyWindow(windowId)) {
        qCInfo(lcWorkspaceCtl) << "workspace move for window" << windowId << "to desktop" << targetDesktopId
                               << "skipped: window is sticky, already on every workspace";
        return false;
    }
    const QString instanceId = PhosphorIdentity::WindowId::extractInstanceId(windowId);
    if (m_windowCensusDesktopIds.value(instanceId).contains(targetDesktopId)) {
        // Already on the target desktop. Only the output can still change,
        // and an output-only move pushes no desktop change for a watch to
        // retire on: arming one would warn two seconds later and hold off the
        // reunion check meanwhile. The reunion cooldown is stamped instead,
        // so the reunion arm does not issue the same output move again while
        // this one is on its way.
        if (!moveOutput) {
            return false;
        }
        m_lastReunionMs.insert(instanceId, m_clock.elapsed());
    } else {
        armMoveWatch(windowId, targetDesktopId);
    }
    Q_EMIT windowWorkspaceMoveRequested(windowId, targetScreenId, desktop, targetDesktopId, direction, moveOutput,
                                        subjectOfVerb);
    return true;
}

void WorkspaceController::runWhenQuiet(std::function<void()> fn)
{
    if (!m_reconciler.hasPendingStructuralOps()) {
        fn();
        return;
    }
    m_quietQueue.append(std::move(fn));
}

void WorkspaceController::drainQuietQueue()
{
    // Re-entrancy: a drained verb can lead back here (a nested settle), and
    // the nested call would walk the member queue a second time and run the
    // re-queued remainder out of order.
    if (m_draining || m_reconciler.hasPendingStructuralOps()) {
        return;
    }
    m_draining = true;
    // Looped rather than one pass, so work queued (or unblocked) during the
    // batch runs in this drain instead of waiting for an unrelated signal.
    while (!m_quietQueue.isEmpty() && !m_reconciler.hasPendingStructuralOps()) {
        const auto queue = std::exchange(m_quietQueue, {});
        for (int i = 0; i < queue.size(); ++i) {
            // Re-checked per verb, not once for the batch. A drained verb can
            // START structural churn of its own (a reorder or an output
            // transfer that makes the reconciler create or remove a desktop),
            // and every verb after it would then resolve its slice index
            // against a map mid-renumber.
            if (m_reconciler.hasPendingStructuralOps()) {
                QList<std::function<void()>> rest = queue.mid(i);
                // Anything a drained verb queued re-entrantly landed in the
                // member queue and belongs AFTER the remainder.
                rest.append(m_quietQueue);
                m_quietQueue = std::move(rest);
                break;
            }
            queue.at(i)();
        }
    }
    // Parked open-path routes drain here too, inside the latch: a re-issued
    // route can start churn of its own (the move fills the destination, which
    // cuts a new trailing empty there), and anything that re-parks waits for
    // the next quiet edge.
    if (!m_reconciler.hasPendingStructuralOps()) {
        drainParkedNamedRoutes();
    }
    m_draining = false;
}

void WorkspaceController::switchScreenToDesktop(const QString& screenId, const QString& desktopId,
                                                PhosphorProtocol::ScreenDesktopSwitchPurpose purpose)
{
    // issueSetCurrent refuses a target that is not in the settled list ahead
    // of its own ledger, so a hopeless switch never opens an entry. A user
    // switch REPLACES a user switch still in flight for the screen: an earlier
    // press the compositor has not answered yet must not eat this one. It
    // never replaces an owner-wins correction, and a correction never
    // replaces anything (the single-correction rule that breaks re-assertion
    // loops), so both are simply refused while one is open.
    const QScopedValueRollback scope(m_switchPurpose, purpose);
    const bool issued = purpose == PhosphorProtocol::ScreenDesktopSwitchPurpose::UserSwitch
        ? m_reconciler.issueSetCurrentReplacing(screenId, desktopId)
        : m_reconciler.issueSetCurrent(screenId, desktopId);
    if (!issued) {
        qCDebug(lcWorkspaceCtl) << "per-screen switch for" << screenId << "to" << desktopId << "refused";
    }
}

void WorkspaceController::focusWorkspace(const QString& screenId, int delta)
{
    runWhenQuiet([this, screenId, delta]() {
        // Key repeat: a press while the previous one is still in flight
        // counts from that press's target, so two quick presses walk two
        // slots instead of the second being refused.
        QString base = m_reconciler.inFlightSetCurrentTarget(screenId);
        if (base.isEmpty()) {
            base = m_reconciler.currentDesktopIdOf(screenId);
        }
        const QString target = m_reconciler.desktopIdAtOffsetFrom(screenId, base, delta);
        if (target.isEmpty()) {
            return; // slice edge: no wrap
        }
        switchScreenToDesktop(screenId, target, PhosphorProtocol::ScreenDesktopSwitchPurpose::UserSwitch);
    });
}

void WorkspaceController::focusWorkspaceAt(const QString& screenId, int sliceIndex)
{
    runWhenQuiet([this, screenId, sliceIndex]() {
        const QString target = m_reconciler.desktopIdAtSliceIndex(screenId, sliceIndex);
        if (target.isEmpty()) {
            return; // slot beyond the screen's slice
        }
        switchScreenToDesktop(screenId, target, PhosphorProtocol::ScreenDesktopSwitchPurpose::UserSwitch);
    });
}

void WorkspaceController::moveWindowToWorkspace(const QString& screenId, const QString& windowId, int delta)
{
    if (windowId.isEmpty()) {
        return;
    }
    // The workspace the user pressed the chord on, captured now: if the move
    // defers behind churn, the screen may show another workspace by the time
    // it runs, and counting from that one would move the window somewhere the
    // user did not ask for.
    const QString sourceId = m_reconciler.currentDesktopIdOf(screenId);
    runWhenQuiet([this, screenId, windowId, delta, sourceId]() {
        if (sourceId.isEmpty() || m_reconciler.map().ownerOf(sourceId) != screenId) {
            return; // unknown, or no longer this screen's workspace
        }
        const QString target = m_reconciler.desktopIdAtOffsetFrom(screenId, sourceId, delta);
        if (target.isEmpty()) {
            return;
        }
        const int desktop = m_vdm->desktopIndexOf(target);
        if (desktop <= 0) {
            return;
        }
        issueWindowMove(windowId, screenId, desktop, target, delta < 0 ? QStringLiteral("up") : QStringLiteral("down"),
                        /*moveOutput=*/true, /*subjectOfVerb=*/true);
    });
}

void WorkspaceController::moveColumnToWorkspace(const QString& screenId, const QString& stripScreenId,
                                                const QStringList& columnWindows, int delta)
{
    if (columnWindows.isEmpty()) {
        return;
    }
    const QString sourceId = m_reconciler.currentDesktopIdOf(screenId);
    runWhenQuiet([this, screenId, stripScreenId, columnWindows, delta, sourceId]() {
        if (sourceId.isEmpty() || m_reconciler.map().ownerOf(sourceId) != screenId) {
            return;
        }
        const QString target = m_reconciler.desktopIdAtOffsetFrom(screenId, sourceId, delta);
        if (target.isEmpty()) {
            return;
        }
        const int desktop = m_vdm->desktopIndexOf(target);
        if (desktop <= 0) {
            return;
        }
        // A sticky member of the column is refused on its own and the rest of
        // the column still moves; the stash records only what moves.
        QStringList moving;
        moving.reserve(columnWindows.size());
        for (const QString& windowId : columnWindows) {
            if (!isStickyWindow(windowId)) {
                moving.append(windowId);
            }
        }
        if (moving.isEmpty()) {
            return;
        }
        // Each window is released from the strip and re-adopted on its own
        // when it arrives, so the column survives the move only through the
        // scroll engine's stash record, which the arrivals claim to rejoin
        // one column. It reads the live column, so it is recorded before the
        // first move releases anything. The handler leaves out a window the
        // move verb refuses at the time it runs, the one being dragged
        // (WindowTrackingAdaptor::stashScrollColumnForWorkspaceMove), which
        // this controller cannot see (C75).
        if (m_columnStashHandler) {
            m_columnStashHandler(stripScreenId, moving, desktop);
        }
        const QString direction = delta < 0 ? QStringLiteral("up") : QStringLiteral("down");
        for (const QString& windowId : std::as_const(moving)) {
            issueWindowMove(windowId, screenId, desktop, target, direction, /*moveOutput=*/true,
                            /*subjectOfVerb=*/true);
        }
    });
}

void WorkspaceController::moveWorkspace(const QString& screenId, int delta)
{
    runWhenQuiet([this, screenId, delta]() {
        m_reconciler.reorderCurrentWorkspace(screenId, delta);
    });
}

void WorkspaceController::moveWorkspaceToOutput(const QString& screenId, NavigationDirection direction)
{
    if (direction != NavigationDirection::Left && direction != NavigationDirection::Right) {
        return;
    }
    runWhenQuiet([this, screenId, direction]() {
        // Neighbour in screen order (geometry left-to-right, the same order
        // the slices concatenate in).
        const QStringList order = m_reconciler.map().screenOrder();
        const int index = order.indexOf(screenId);
        if (index < 0) {
            return;
        }
        const int neighbourIndex = direction == NavigationDirection::Left ? index - 1 : index + 1;
        if (neighbourIndex < 0 || neighbourIndex >= order.size()) {
            return;
        }
        const QString targetScreen = order.at(neighbourIndex);

        // Windows riding along are enumerated BEFORE the transfer (the census
        // keys by desktop id, unaffected by the map mutation). A window placed
        // on several desktops stays: moving its output would take it off the
        // others as well.
        const QString movingId = m_reconciler.currentDesktopIdOf(screenId);
        QStringList riders;
        for (auto it = m_windowCensusDesktopIds.constBegin(); it != m_windowCensusDesktopIds.constEnd(); ++it) {
            if (it.value().size() == 1 && it.value().constFirst() == movingId) {
                riders.append(it.key());
            }
        }

        const QString movedId = m_reconciler.transferCurrentWorkspace(screenId, targetScreen);
        if (movedId.isEmpty()) {
            return;
        }
        const int desktop = m_vdm->desktopIndexOf(movedId);
        if (desktop <= 0) {
            return;
        }
        // The desktop keeps its identity; the windows change OUTPUT. The
        // handoff verb re-homes each one's engine state and geometry on the
        // target screen. The move names the desktop by id so a renumber
        // between this emit and the compositor acting on it cannot send a
        // rider to whatever took the old number. Riders are collateral, not
        // the subject of the verb.
        // Each rider is released from its strip and re-adopted on its own when
        // it arrives, so a scrolling strip survives the move only through the
        // record the arrivals claim. It reads the live strip, so it is taken
        // before the first move releases anything.
        if (m_stripStashHandler && !riders.isEmpty()) {
            m_stripStashHandler(riders, targetScreen, desktop);
        }
        const QString directionText =
            direction == NavigationDirection::Left ? QStringLiteral("left") : QStringLiteral("right");
        for (const QString& windowId : std::as_const(riders)) {
            issueWindowMove(windowId, targetScreen, desktop, movedId, directionText, /*moveOutput=*/true,
                            /*subjectOfVerb=*/false);
        }
        // niri semantics: the moved workspace gains focus on its new output
        // (the source already switched inside the transfer).
        switchScreenToDesktop(targetScreen, movedId, PhosphorProtocol::ScreenDesktopSwitchPurpose::UserSwitch);
    });
}

// ── Named workspaces ────────────────────────────────────────────────────────

void WorkspaceController::applyNamedDeclarations(const QVariantList& entries)
{
    m_namedEntries = entries;
    // The identity half is refreshed at once, so the named verbs and the
    // rule resolver see a rename the moment the config carries it. The
    // validator already trimmed, deduplicated and bounded every entry.
    m_namedDeclarations.clear();
    m_namedDeclarations.reserve(entries.size());
    for (const QVariant& value : entries) {
        const QVariantMap map = value.toMap();
        m_namedDeclarations.append(NamedDeclaration{map.value(ConfigDefaults::namedEntryIdField()).toString(),
                                                    map.value(ConfigDefaults::namedEntryNameField()).toString()});
    }
    if (!m_adopted) {
        return; // applied by the post-adoption tail
    }
    m_namedApplied = true;
    // One body at a time: during churn every settle would queue another, and
    // the one waiting already reads the newest entries when it runs.
    if (m_namedApplyQueued) {
        return;
    }
    m_namedApplyQueued = true;
    runWhenQuiet([this]() {
        m_namedApplyQueued = false;
        QList<PhosphorWorkspaces::NamedWorkspace> declarations;
        declarations.reserve(m_namedEntries.size());
        for (const QVariant& value : std::as_const(m_namedEntries)) {
            const QVariantMap map = value.toMap();
            PhosphorWorkspaces::NamedWorkspace decl;
            decl.id = map.value(ConfigDefaults::namedEntryIdField()).toString();
            decl.name = map.value(ConfigDefaults::namedEntryNameField()).toString();
            // The settings UI stores an EFFECTIVE screen id: a "<phys>/vs:N"
            // virtual id on a subdivided output, or a bare connector name in
            // the Qt fallback. The map only knows physical, effect-reported
            // ids, so the raw value is normalized like every other externally
            // sourced screen id.
            const QString rawOutput = map.value(ConfigDefaults::namedEntryOutputField()).toString();
            decl.outputId = rawOutput.isEmpty()
                ? QString()
                : canonicalScreenId(PhosphorIdentity::VirtualScreenId::extractPhysicalId(rawOutput));
            if (!decl.outputId.isEmpty() && !m_reconciler.map().knowsScreen(decl.outputId)) {
                // Not fatal: the monitor may simply be unplugged, and the pin
                // takes effect when it returns. Said once per pin, not on
                // every settle that re-applies the declarations.
                const QString key = decl.name + QLatin1Char('\n') + decl.outputId;
                if (!m_reportedUnknownPins.contains(key)) {
                    m_reportedUnknownPins.insert(key);
                    qCInfo(lcWorkspaceCtl) << "named workspace" << decl.name << "pins output" << rawOutput
                                           << "(resolved to" << decl.outputId << ") which is not connected";
                }
            }
            decl.position = map.value(ConfigDefaults::namedEntryPositionField(), -1).toInt();
            declarations.append(decl);
        }
        // rawDesktopNames, NOT desktopNames: this is the IDENTITY path, and it
        // needs KWin's names verbatim. desktopNames() fills placeholders for
        // DISPLAY callers.
        m_reconciler.applyNamedWorkspaces(declarations, m_vdm->rawDesktopNames());
        // Routes parked while this body waited can resolve now.
        if (!m_reconciler.hasPendingStructuralOps()) {
            drainParkedNamedRoutes();
        }
    });
}

const WorkspaceController::NamedDeclaration* WorkspaceController::findDeclaration(const QString& entryId,
                                                                                  const QString& name) const
{
    if (!entryId.isEmpty()) {
        for (const NamedDeclaration& declaration : m_namedDeclarations) {
            if (!declaration.id.isEmpty() && sameEntryId(declaration.id, entryId)) {
                return &declaration;
            }
        }
    }
    if (!name.isEmpty()) {
        for (const NamedDeclaration& declaration : m_namedDeclarations) {
            if (declaration.name == name) {
                return &declaration;
            }
        }
    }
    return nullptr;
}

QString WorkspaceController::desktopIdForDeclaration(const NamedDeclaration& declaration) const
{
    // Adoption gate. Before adoption the map is NOT empty: start() loads the
    // previous session's candidate map from the state file, names and all,
    // and routing on it sends the window to whatever position that id happens
    // to occupy in this session's list.
    if (!m_adopted) {
        return QString();
    }
    const QStringList ids = m_reconciler.map().allDesktopIds();
    // The reconciler stamps the declaration's id on the desktop it realizes,
    // which is what survives a rename.
    if (!declaration.id.isEmpty()) {
        for (const QString& id : ids) {
            if (m_reconciler.map().entryFor(id).namedId == declaration.id) {
                return id;
            }
        }
    }
    // Tie-break for the name match: the FIRST match in the map's canonical
    // order wins, and a second match is reported. The reconciler collapses
    // duplicate map names, so a duplicate here is transient.
    QString match;
    for (const QString& id : ids) {
        if (m_reconciler.map().entryFor(id).name != declaration.name) {
            continue;
        }
        if (match.isEmpty()) {
            match = id;
            continue;
        }
        qCWarning(lcWorkspaceCtl) << "more than one workspace is named" << declaration.name << "- using" << match
                                  << "and ignoring" << id;
        break;
    }
    return match;
}

void WorkspaceController::focusNamedWorkspace(const QString& entryId, const QString& name)
{
    runWhenQuiet([this, entryId, name]() {
        const NamedDeclaration* declaration = findDeclaration(entryId, name);
        if (!declaration) {
            return;
        }
        const QString target = desktopIdForDeclaration(*declaration);
        if (target.isEmpty()) {
            return;
        }
        // A named workspace shows where it LIVES: the switch targets its
        // owner screen, whichever screen the shortcut fired on.
        const QString owner = m_reconciler.map().ownerOf(target);
        if (owner.isEmpty()) {
            qCWarning(lcWorkspaceCtl) << "focus named workspace" << declaration->name
                                      << ": no owner screen yet, ignoring";
            return;
        }
        switchScreenToDesktop(owner, target, PhosphorProtocol::ScreenDesktopSwitchPurpose::UserSwitch);
    });
}

int WorkspaceController::routeWindowToNamedWorkspace(const QString& entryId, const QString& name,
                                                     const QString& windowId, bool moveOutput, QString* ownerScreenOut)
{
    // Rules-pipeline arm: no runWhenQuiet, the caller needs the truth NOW.
    if (windowId.isEmpty()) {
        return static_cast<int>(WorkspaceRouteVerdict::Unrealized);
    }
    // Undeclared: the positional route is the author's fallback for exactly
    // this, so it must stay available. Checked first, so nothing below parks
    // a target no declaration names.
    const NamedDeclaration* declaration = findDeclaration(entryId, name);
    if (!declaration) {
        return static_cast<int>(WorkspaceRouteVerdict::Unrealized);
    }
    const QString instanceId = PhosphorIdentity::WindowId::extractInstanceId(windowId);
    const auto park = [&]() {
        m_parkedNamedRoutes.insert(instanceId, ParkedNamedRoute{declaration->id, declaration->name, moveOutput});
        return static_cast<int>(WorkspaceRouteVerdict::Unresolvable);
    };
    // Before adoption the map is the previous session's candidate, and that
    // window is exactly the login / session-restore population these rules
    // are written for. During structural churn the slice index cannot be
    // promised, and the op in flight is usually the one this very window
    // started (a filled workspace cuts a new trailing empty). And until the
    // declarations are applied the map may not carry this one yet. All three
    // park and re-issue once the reconciler is quiet and applied.
    if (!m_adopted || !m_namedApplied || m_namedApplyQueued || m_reconciler.hasPendingStructuralOps()) {
        return park();
    }
    const QString target = desktopIdForDeclaration(*declaration);
    if (target.isEmpty()) {
        // Declared, quiet and applied, yet not realized (a create KWin
        // refused at its cap). The positional route must not stand in.
        return static_cast<int>(WorkspaceRouteVerdict::Unresolvable);
    }
    const int desktop = m_vdm->desktopIndexOf(target);
    if (desktop <= 0) {
        // Realized, but the list carrying its number has not committed yet;
        // the next settled list re-issues it.
        return park();
    }
    const QString owner = m_reconciler.map().ownerOf(target);
    // A route that issues nothing (a sticky window, or one already on the
    // workspace with no output leg asked for) reports the REALIZED desktop so
    // the caller does not fall back to an unrelated positional route, but
    // names no owner screen: the window stays on the output it opened on, and
    // the caller pins the placement to whatever owner it is handed.
    if (!issueWindowMove(windowId, owner, desktop, target, QStringLiteral("down"), moveOutput,
                         /*subjectOfVerb=*/true)) {
        return desktop;
    }
    if (ownerScreenOut) {
        *ownerScreenOut = owner;
    }
    return desktop;
}

void WorkspaceController::moveWindowToNamedWorkspace(const QString& entryId, const QString& name,
                                                     const QString& windowId)
{
    if (windowId.isEmpty()) {
        return;
    }
    runWhenQuiet([this, entryId, name, windowId]() {
        const NamedDeclaration* declaration = findDeclaration(entryId, name);
        if (!declaration) {
            return;
        }
        const QString target = desktopIdForDeclaration(*declaration);
        if (target.isEmpty()) {
            return;
        }
        const int desktop = m_vdm->desktopIndexOf(target);
        if (desktop <= 0) {
            return;
        }
        issueWindowMove(windowId, m_reconciler.map().ownerOf(target), desktop, target, QStringLiteral("down"),
                        /*moveOutput=*/true, /*subjectOfVerb=*/true);
    });
}

void WorkspaceController::drainParkedNamedRoutes()
{
    // Nothing resolves before adoption, and a route drained while the
    // declarations wait to be applied would read a map that may not carry
    // its workspace yet and fall to Unresolvable for good.
    if (m_parkedNamedRoutes.isEmpty() || !m_adopted || m_namedApplyQueued) {
        return;
    }
    const QHash<QString, ParkedNamedRoute> parked = std::exchange(m_parkedNamedRoutes, {});
    for (auto it = parked.constBegin(); it != parked.constEnd(); ++it) {
        // Windows that went away while the route waited have nothing to
        // route. The registry is keyed by instance id, which is how the park
        // stored them.
        if (!m_registry->metadata(it.key())) {
            continue;
        }
        qCInfo(lcWorkspaceCtl) << "re-issuing the parked workspace route for" << it.key() << "to" << it.value().name;
        routeWindowToNamedWorkspace(it.value().entryId, it.value().name, it.key(), it.value().moveOutput);
    }
}

} // namespace PlasmaZones
