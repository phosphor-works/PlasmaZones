// SPDX-FileCopyrightText: 2026 fuddlesworth
// SPDX-License-Identifier: LGPL-2.1-or-later

// The screen-lifecycle half of WorkspaceReconciler: hotplug (add, remove,
// rename, reorder, and the home-stamp migration that carries an output's
// workspaces back when it returns) and first-run/restart adoption. Split out of
// WorkspaceReconciler.cpp on file size, the same way WorkspaceReconcilerNamed
// .cpp is. The state and every helper used here are declared in the one
// header, and the logging category is defined in WorkspaceReconciler.cpp.

#include <PhosphorWorkspaces/WorkspaceReconciler.h>

#include <QLoggingCategory>
#include <QSet>

#include <utility>

Q_DECLARE_LOGGING_CATEGORY(lcWorkspaceRec)

namespace PhosphorWorkspaces {

namespace {
/// Move a per-screen record from one key to another. A record already under
/// the new key wins and the old key's is dropped: it was written for the new
/// spelling, so it is the fresher of the two (VirtualDesktopManager::
/// renameScreen keeps an existing target row the same way).
template<typename Hash>
void rekey(Hash& hash, const QString& from, const QString& to)
{
    const auto it = hash.find(from);
    if (it == hash.end()) {
        return;
    }
    auto value = it.value();
    hash.erase(it);
    if (!hash.contains(to)) {
        hash.insert(to, value);
    }
}
} // namespace

// ── Screens ─────────────────────────────────────────────────────────────────

QSet<QString> WorkspaceReconciler::migrateHome(const QString& screenId)
{
    // Every entry whose homeScreenId names this output moves back, in its
    // current relative order, ahead of whatever the output already holds.
    QSet<QString> touchedScreens;
    QStringList returning;
    const QStringList owned = m_map.allDesktopIds();
    for (const QString& id : owned) {
        if (m_map.entryFor(id).homeScreenId != screenId) {
            continue;
        }
        if (m_map.ownerOf(id) == screenId) {
            // Already home; a stamp naming its own owner is no displacement.
            m_map.setHomeScreen(id, QString());
            continue;
        }
        returning.append(id);
    }
    int index = 0;
    for (const QString& id : std::as_const(returning)) {
        touchedScreens.insert(m_map.ownerOf(id));
        m_map.transfer(id, screenId, index++);
        m_map.setHomeScreen(id, QString());
    }
    return touchedScreens;
}

void WorkspaceReconciler::onScreenAdded(const QString& screenId)
{
    // hasScreen, not knowsScreen: the two real signal orders a hotplug arrives
    // in disagree about which of these runs first. When the controller's
    // recomputed screen order lands BEFORE the screenAdded, the new output is
    // already in the screen order by the time we get here, so knowsScreen
    // answered true and the screen was returned from — never migrated home,
    // never given a desktop. Holding a SLICE is the real "already dealt with"
    // test, and setScreenOrder de-duplicates.
    if (screenId.isEmpty() || m_map.hasScreen(screenId)) {
        return;
    }
    m_map.setScreenOrder(m_map.screenOrder() << screenId);

    const QSet<QString> touchedScreens = migrateHome(screenId);

    // The last screen's slice, parked when it went away with nowhere to go.
    // Its entries come back with their names and their own home stamps (one
    // homed here is simply home again).
    if (!m_parked.isEmpty()) {
        const QSet<QString> live(m_lastIds.cbegin(), m_lastIds.cend());
        const QList<WorkspaceEntry> parked = std::exchange(m_parked, {});
        for (WorkspaceEntry entry : parked) {
            if ((!live.isEmpty() && !live.contains(entry.desktopId)) || !m_map.ownerOf(entry.desktopId).isEmpty()) {
                continue;
            }
            if (entry.homeScreenId == screenId) {
                entry.homeScreenId.clear();
            }
            m_map.insert(screenId, m_map.sliceSize(screenId), entry);
        }
    }
    // Whatever KWin still has that nothing owns (a create that landed while no
    // screen existed) belongs to the screen that just arrived. Without this,
    // a session that lost its last output asked KWin for a brand new desktop
    // on replug while every existing one sat unowned.
    if (m_adopted) {
        for (const QString& id : std::as_const(m_lastIds)) {
            if (m_map.ownerOf(id).isEmpty()) {
                WorkspaceEntry entry;
                entry.desktopId = id;
                m_map.insert(screenId, kwinOrderedSlot(screenId, id), entry);
            }
        }
    }

    maintainScreen(screenId);
    for (const QString& touched : touchedScreens) {
        maintainScreen(touched); // the foster screen's invariants re-settle
        // The foster screen may have been SHOWING one of the migrated
        // desktops; it now sits on a foreign one and needs the owner-wins
        // correction, exactly as the verb-side transfer path does.
        evaluateForeign(touched);
    }
    // The arriving screen can already be showing a desktop someone else owns
    // (its current was seeded before it joined the map).
    evaluateForeign(screenId);
    bumpGeneration();
}

void WorkspaceReconciler::onScreenRemoved(const QString& screenId)
{
    if (!m_map.knowsScreen(screenId)) {
        return;
    }
    const QList<WorkspaceEntry> orphaned = m_map.takeSlice(screenId);
    m_currentIdByScreen.remove(screenId);
    m_pendingReportByScreen.remove(screenId);
    // The snap-back memo names a desktop this screen no longer owns; keeping
    // it would aim a later correction (after a replug re-adds the screen) at
    // whatever that desktop became in the meantime.
    m_lastOwnedByScreen.remove(screenId);
    // Per-screen budgets go with the screen: a re-added output deserves a
    // fresh set of attempts rather than the spent budget of its predecessor.
    m_createRefusals.remove(screenId);
    m_setCurrentRefusals.remove(screenId);
    // A per-screen switch for a gone output can never be echoed, and until it
    // expired it would block the corrections of a re-added screen with the
    // same id. Creates stay: KWin will still answer them, and dropping one
    // turned its echo into an unmatched creation that could pair with
    // another screen's create. Its landing finds the screen gone and goes to
    // the adoption target (realizeCreate).
    for (int i = m_ledger.size() - 1; i >= 0; --i) {
        if (m_ledger.at(i).kind == PendingOp::Kind::SetCurrent && m_ledger.at(i).screenId == screenId) {
            m_ledger.removeAt(i);
        }
    }
    stopLedgerTimerIfIdle();
    if (m_focusedScreen == screenId) {
        m_focusedScreen.clear();
    }

    // Each entry is stamped with its home output for migrate-back on replug.
    // A second displacement keeps the ORIGINAL home (the entry already
    // carries one), so daisy-chained unplugs still return the workspace to
    // where the user put it.
    const QStringList order = m_map.screenOrder();
    const QString fallback = order.isEmpty() ? QString() : order.first();
    if (fallback.isEmpty()) {
        // Last screen went away. KWin keeps the desktops, so the slice is
        // parked rather than dropped, and the next screenAdded takes it back
        // with names and stamps intact.
        for (WorkspaceEntry entry : orphaned) {
            if (entry.homeScreenId.isEmpty()) {
                entry.homeScreenId = screenId;
            }
            m_parked.append(entry);
        }
        bumpGeneration();
        return;
    }
    // Before the fallback's trailing empty, preserving order. These entries
    // are exempt from the destroy sweep while displaced (isReapable), so the
    // output gets its workspaces back exactly as it left them.
    int index = insertIndexBeforeTrailingEmpty(fallback);
    for (auto entry : orphaned) {
        if (entry.homeScreenId.isEmpty()) {
            entry.homeScreenId = screenId;
        }
        m_map.insert(fallback, index++, entry);
    }
    maintainScreen(fallback);
    evaluateForeign(fallback);
    bumpGeneration();
}

void WorkspaceReconciler::onScreenRenamed(const QString& fromScreenId, const QString& toScreenId)
{
    if (fromScreenId.isEmpty() || toScreenId.isEmpty() || fromScreenId == toScreenId) {
        return;
    }
    const bool known = m_map.knowsScreen(fromScreenId);
    if (known && m_map.hasScreen(toScreenId)) {
        // Both spellings hold a slice: the same output was already tracked
        // under the new id. Fold the old slice in before the trailing empty,
        // unstamped, since nothing was displaced.
        const QList<WorkspaceEntry> entries = m_map.takeSlice(fromScreenId);
        int index = insertIndexBeforeTrailingEmpty(toScreenId);
        for (WorkspaceEntry entry : entries) {
            if (entry.homeScreenId == fromScreenId || entry.homeScreenId == toScreenId) {
                entry.homeScreenId.clear();
            }
            m_map.insert(toScreenId, index++, entry);
        }
        const QStringList owned = m_map.allDesktopIds();
        for (const QString& id : owned) {
            if (m_map.entryFor(id).homeScreenId == fromScreenId) {
                m_map.setHomeScreen(id, toScreenId);
            }
        }
    } else if (known) {
        m_map.renameScreen(fromScreenId, toScreenId);
    }

    // Every per-screen record follows the id, so the renamed output keeps its
    // current desktop, its snap-back memo, its budgets and its open ops. Where
    // the new id already has a record of its own (the effect reported under
    // the new spelling before this rename arrived), that one is kept.
    rekey(m_currentIdByScreen, fromScreenId, toScreenId);
    rekey(m_pendingReportByScreen, fromScreenId, toScreenId);
    rekey(m_lastOwnedByScreen, fromScreenId, toScreenId);
    rekey(m_createRefusals, fromScreenId, toScreenId);
    rekey(m_setCurrentRefusals, fromScreenId, toScreenId);
    // At most one switch is open per screen (openSetCurrentIndex): one the
    // new id already has keeps the screen, and the old id's is dropped rather
    // than renamed beside it. The kept switch owes nothing for the dropped
    // one. A switch under the new id means the effect has already re-spelled
    // the output (a report or a verb under the new id came from it), and its
    // reports and verbs arrive in order on one connection, so no answer under
    // the old spelling can follow. Creates are re-homed whatever, since KWin
    // answers every one of them.
    const bool toHasSwitch = openSetCurrentIndex(toScreenId) >= 0;
    for (int i = m_ledger.size() - 1; i >= 0; --i) {
        PendingOp& op = m_ledger[i];
        if (op.screenId != fromScreenId) {
            continue;
        }
        if (op.kind == PendingOp::Kind::SetCurrent && toHasSwitch) {
            m_ledger.removeAt(i);
            continue;
        }
        op.screenId = toScreenId;
    }
    stopLedgerTimerIfIdle();
    for (WorkspaceEntry& entry : m_parked) {
        if (entry.homeScreenId == fromScreenId) {
            entry.homeScreenId = toScreenId;
        }
    }
    if (m_focusedScreen == fromScreenId) {
        m_focusedScreen = toScreenId;
    }
    if (!known) {
        return;
    }

    // Entries stamped home to the old id name an output that is connected
    // right now under its new id, so they go home at once.
    const QSet<QString> touchedScreens = migrateHome(toScreenId);
    maintainScreen(toScreenId);
    for (const QString& touched : touchedScreens) {
        maintainScreen(touched);
        evaluateForeign(touched);
    }
    evaluateForeign(toScreenId);
    bumpGeneration();
}

void WorkspaceReconciler::onScreenOrderChanged(const QStringList& order)
{
    const QStringList before = m_map.screenOrder();
    if (order == before) {
        return;
    }
    m_map.setScreenOrder(order);
    // setScreenOrder REPAIRS the order it is handed (slice-holding screens
    // missing from it are appended), so a caller-order that differs can still
    // repair back to what we already had. Compare the result, not the request.
    if (m_map.screenOrder() == before) {
        return;
    }
    bumpGeneration();
}

// ── Adoption ────────────────────────────────────────────────────────────────

bool WorkspaceReconciler::adoptAll(const QStringList& ids, const QHash<QString, QString>& currentDesktopIdByScreen,
                                   const QHash<QString, QString>& preferredScreenByDesktopId,
                                   const QStringList& rawNames)
{
    // KWin never has zero desktops, and a world with no screen has nowhere to
    // put one. Either is a read that has not completed: repairAgainst on an
    // empty list would wipe the restored candidate, and adopting onto no
    // screen leaves everything unowned. Refused, and still unadopted, so the
    // caller can retry once the read is real.
    if (ids.isEmpty()) {
        qCWarning(lcWorkspaceRec) << "refusing to adopt an empty desktop list";
        return false;
    }
    const QStringList order = m_map.screenOrder();
    if (order.isEmpty()) {
        qCWarning(lcWorkspaceRec) << "refusing to adopt" << ids.size() << "desktops with no screen to adopt onto";
        return false;
    }
    m_lastIds = ids;
    m_lastRawNames = rawNames.size() == ids.size() ? rawNames : QStringList();

    // Keep consistent restored content; repair drops vanished ids, and what
    // still needs an owner is distributed below.
    m_map.repairAgainst(ids);
    // A restored entry displaced last session whose output is connected now
    // goes home here: nothing else would ever move it, since the output never
    // produces a screenAdded for a session it was present from the start of.
    for (const QString& screenId : order) {
        migrateHome(screenId);
    }

    // Seed the per-screen current from what the caller observed. Without it
    // every verb resolves empty until the effect's first per-output report
    // happens along. A report that came in before adoption is the fallback
    // for a screen the caller has nothing for.
    for (const QString& screenId : order) {
        QString shownId = currentDesktopIdByScreen.value(screenId);
        if (shownId.isEmpty() || !ids.contains(shownId)) {
            const int pending = m_pendingReportByScreen.value(screenId, 0);
            shownId = (pending >= 1 && pending <= ids.size()) ? ids.at(pending - 1) : QString();
        }
        m_pendingReportByScreen.remove(screenId);
        if (!shownId.isEmpty()) {
            m_currentIdByScreen.insert(screenId, shownId);
        }
    }

    // Pass 1: each desktop currently shown by a screen goes to that screen;
    // ties resolve to the first screen in order. Every insert lands at the
    // slot that keeps the slice in KWin order.
    for (const QString& screenId : order) {
        const QString shownId = m_currentIdByScreen.value(screenId);
        if (shownId.isEmpty()) {
            continue;
        }
        if (m_map.ownerOf(shownId).isEmpty()) {
            WorkspaceEntry entry;
            entry.desktopId = shownId;
            m_map.insert(screenId, kwinOrderedSlot(screenId, shownId), entry);
        }
        if (m_map.ownerOf(shownId) == screenId) {
            m_lastOwnedByScreen.insert(screenId, shownId);
        }
    }

    // Pass 2: a desktop no screen shows goes where the caller saw its windows.
    // Under KWin's global mode every output shows the same desktop, so a
    // first adoption would otherwise hand every other desktop to whichever
    // screen precedes it in KWin's order, and the owner-wins reunion would
    // then move each window to that screen. A preference naming a screen
    // outside the order is ignored.
    for (const QString& id : ids) {
        if (!m_map.ownerOf(id).isEmpty()) {
            continue;
        }
        const QString preferred = preferredScreenByDesktopId.value(id);
        if (preferred.isEmpty() || !order.contains(preferred)) {
            continue;
        }
        WorkspaceEntry entry;
        entry.desktopId = id;
        m_map.insert(preferred, kwinOrderedSlot(preferred, id), entry);
    }

    // Pass 3: remaining desktops keep KWin order contiguous — each unowned id
    // joins the owner of the nearest preceding owned desktop in global order;
    // a leading run goes to the first screen.
    for (int globalIdx = 0; globalIdx < ids.size(); ++globalIdx) {
        const QString& id = ids.at(globalIdx);
        if (!m_map.ownerOf(id).isEmpty()) {
            continue;
        }
        QString target;
        for (int i = globalIdx - 1; i >= 0 && target.isEmpty(); --i) {
            target = m_map.ownerOf(ids.at(i));
        }
        if (target.isEmpty()) {
            target = order.first();
        }
        WorkspaceEntry entry;
        entry.desktopId = id;
        m_map.insert(target, kwinOrderedSlot(target, id), entry);
    }

    m_adopted = true;
    maintainInvariants();
    // KWin never relabels a desktop after creating it, so a create or remove
    // made before adoption (in an earlier session, or with the feature off)
    // can already have left a positional label naming another position. The
    // settle path keeps them current from here on, but no settle comes for a
    // list that does not change.
    refreshPositionalLabels();
    // Owner-wins over the seeded currents: a screen that came up showing a
    // desktop a restored map gives to another screen needs the correction now,
    // not at its next report.
    evaluateForeignAll();
    bumpGeneration();
    return true;
}

} // namespace PhosphorWorkspaces
