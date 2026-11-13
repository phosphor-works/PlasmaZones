// SPDX-FileCopyrightText: 2026 fuddlesworth
// SPDX-License-Identifier: LGPL-2.1-or-later

// The named-workspace half of WorkspaceReconciler: realizing each declaration
// (match by id, rename in place, claim by KWin name, create, pin, position) and
// the revert-to-dynamic sweep. Split out of WorkspaceReconciler.cpp on file
// size. The state and every helper used here are declared in the one header,
// and the logging category is defined in WorkspaceReconciler.cpp.

#include <PhosphorWorkspaces/WorkspaceReconciler.h>

#include <QLoggingCategory>
#include <QSet>

#include <algorithm>

Q_DECLARE_LOGGING_CATEGORY(lcWorkspaceRec)

namespace PhosphorWorkspaces {

int WorkspaceReconciler::namedSliceIndex(const QString& screenId, int declaredPosition) const
{
    // The trailing empty is an invariant, not a slot users place things after:
    // a named workspace landing behind it makes the empty desktop mid-slice,
    // which maintenance then reaps, and the named entry inherits the trailing
    // role it is exempt from. Everything a declaration can ask for is clamped
    // to the last position BEFORE it.
    const int last = insertIndexBeforeTrailingEmpty(screenId);
    if (declaredPosition < 0) {
        return last;
    }
    return qBound(0, declaredPosition, last);
}

void WorkspaceReconciler::applyNamedWorkspaces(const QList<NamedWorkspace>& declarations, const QStringList& kwinNames)
{
    m_declarations = declarations;
    m_declarationKwinNames = kwinNames;
    if (!m_adopted) {
        return; // the map is still a candidate; adoption's caller re-applies
    }
    bool changed = false;

    // Every live declaration id, up front: an entry stamped with the id of a
    // declaration further down the list must not be taken by an earlier one
    // that merely shares its name (two workspaces swapping names).
    QSet<QString> liveIds;
    for (const NamedWorkspace& decl : declarations) {
        if (!decl.id.isEmpty()) {
            liveIds.insert(decl.id);
        }
    }

    const auto kwinNameAt = [this, &kwinNames](const QString& desktopId) {
        const int position = static_cast<int>(m_lastIds.indexOf(desktopId));
        return (position >= 0 && position < kwinNames.size()) ? kwinNames.at(position) : QString();
    };
    const auto kwinCovers = [this, &kwinNames](const QString& desktopId) {
        const int position = static_cast<int>(m_lastIds.indexOf(desktopId));
        return position >= 0 && position < kwinNames.size();
    };
    // A desktop whose KWin name is `name`, owned, unnamed in the map and not
    // already taken this pass. A positional label never identifies anything.
    const auto findByKWinName = [&](const QString& name, const QSet<QString>& taken, const QString& skip) {
        if (isPositionalLabel(name)) {
            return QString();
        }
        for (int i = 0; i < m_lastIds.size() && i < kwinNames.size(); ++i) {
            const QString& id = m_lastIds.at(i);
            if (id == skip || taken.contains(id) || kwinNames.at(i) != name) {
                continue;
            }
            // Only a desktop the MAP owns can be claimed: setName on an
            // unowned id is a silent no-op, so claiming one would re-run the
            // same futile claim on every apply.
            if (!m_map.ownerOf(id).isEmpty() && m_map.entryFor(id).name.isEmpty()) {
                return id;
            }
        }
        return QString();
    };

    struct Positioned
    {
        QString desktopId;
        int position;
        int row;
    };
    QHash<QString, QList<Positioned>> positionedByScreen;

    // Pass 1: realize every declaration, in row order.
    QSet<QString> claimed;
    QSet<QString> declaredNames;
    QSet<QString> seenIds;
    for (int row = 0; row < declarations.size(); ++row) {
        const NamedWorkspace& decl = declarations.at(row);
        // Trimmed, because the declaration comes from config: a name that is
        // all whitespace renders as nothing in every UI yet would still make
        // its desktop destroy-exempt.
        const QString declaredName = decl.name.trimmed();
        const bool duplicateId = !decl.id.isEmpty() && seenIds.contains(decl.id);
        if (declaredName.isEmpty() || declaredNames.contains(declaredName) || duplicateId) {
            // Config-fed and normally rejected by the settings UI, so reaching
            // here means the config was hand-edited or a UI guard regressed —
            // worth saying out loud rather than dropping in silence.
            qCWarning(lcWorkspaceRec) << "skipping"
                                      << (declaredName.isEmpty() ? "an empty"
                                                                 : (duplicateId ? "a duplicate-id" : "a duplicate"))
                                      << "named-workspace declaration:" << decl.name;
            continue;
        }
        declaredNames.insert(declaredName);
        if (!decl.id.isEmpty()) {
            seenIds.insert(decl.id);
        }

        // Already realized? By persistent id first, which is what makes a
        // rename in settings rename the desktop in place rather than create a
        // fresh one and revert the old.
        QString realizedId;
        const QStringList owned = m_map.allDesktopIds();
        if (!decl.id.isEmpty()) {
            for (const QString& id : owned) {
                if (!claimed.contains(id) && m_map.entryFor(id).namedId == decl.id) {
                    realizedId = id;
                    break;
                }
            }
        }
        if (realizedId.isEmpty()) {
            for (const QString& id : owned) {
                const WorkspaceEntry entry = m_map.entryFor(id);
                if (!claimed.contains(id) && entry.name == declaredName
                    && (entry.namedId.isEmpty() || !liveIds.contains(entry.namedId))) {
                    realizedId = id;
                    break;
                }
            }
        }

        // Claim an unnamed desktop whose KWin name matches (restart without a
        // state file: the name we stamped last session is the identity).
        if (realizedId.isEmpty()) {
            realizedId = findByKWinName(declaredName, claimed, QString());
            if (!realizedId.isEmpty()) {
                m_map.setName(realizedId, declaredName);
                changed = true;
            }
        }

        if (!realizedId.isEmpty()) {
            const WorkspaceEntry entry = m_map.entryFor(realizedId);
            if (entry.namedId != decl.id) {
                m_map.setNamedId(realizedId, decl.id);
                changed = true;
            }
            if (entry.name != declaredName) {
                // Renamed in settings: the same workspace, a new name. Pushed
                // to KWin at once, so its own name list (the identity a
                // restart without a state file claims by) follows.
                m_map.setName(realizedId, declaredName);
                m_reportedKWinRenames.remove(realizedId);
                pushDesktopName(realizedId, declaredName);
                changed = true;
            }
        }

        // Create-echo healing: an id-only create echo matches the oldest open
        // create, so an external creation racing ours can be realized under
        // the declared name while our real desktop adopts elsewhere. KWin's
        // OWN name list is the tiebreaker: when it disagrees about the
        // realized entry and some other desktop carries the declared name,
        // the identity followed the name — move it there.
        if (!realizedId.isEmpty() && kwinCovers(realizedId)) {
            const QString kwinName = kwinNameAt(realizedId);
            if (kwinName == declaredName) {
                // KWin's list now carries the name; whatever we pushed landed.
                m_namePushes.remove(realizedId);
                m_reportedKWinRenames.remove(realizedId);
            } else if (const QString other = findByKWinName(declaredName, claimed, realizedId); !other.isEmpty()) {
                const QString namedId = m_map.entryFor(realizedId).namedId;
                m_map.setName(realizedId, QString());
                m_map.setNamedId(realizedId, QString());
                m_map.setName(other, declaredName);
                m_map.setNamedId(other, namedId);
                m_namePushes.remove(realizedId);
                realizedId = other;
                changed = true;
            } else if (!kwinName.isEmpty() && !isPositionalLabel(kwinName) && !m_namePushes.contains(realizedId)) {
                // A name a user chose, on a desktop we have no push in flight
                // for: renamed on KWin's side (Pager, System Settings). Not
                // pushed back over; adopting it is the controller's call.
                if (m_reportedKWinRenames.value(realizedId) != kwinName) {
                    m_reportedKWinRenames.insert(realizedId, kwinName);
                    Q_EMIT namedWorkspaceRenamedInKWin(decl.id, realizedId, kwinName);
                }
            } else {
                // Nobody else carries the name and KWin shows its positional
                // default (or what we pushed has not landed): KWin is behind,
                // after a rename that never reached it or a restart that
                // dropped it. Push it, so the KWin-name claim can re-identify
                // this desktop next session.
                pushDesktopName(realizedId, declaredName);
            }
        }

        if (realizedId.isEmpty()) {
            // Create it. One create per declaration per pass: an open Create
            // for this declaration means the previous request has not landed.
            bool pending = false;
            for (const auto& op : m_ledger) {
                if (op.kind == PendingOp::Kind::Create && !op.echoed
                    && (decl.id.isEmpty() ? op.name == declaredName : op.namedId == decl.id)) {
                    pending = true;
                    break;
                }
            }
            if (pending) {
                continue;
            }
            QString target =
                (!decl.outputId.isEmpty() && m_map.knowsScreen(decl.outputId)) ? decl.outputId : adoptionTarget();
            if (target.isEmpty()) {
                continue;
            }
            // Cap-guarded and budgeted inside. At the desktop cap nothing is
            // requested, but the declaration is kept, not refused or dropped:
            // the controller re-applies every declaration on each settled
            // desktop list, so once a removal frees room this pass creates it.
            requestCreateAt(target, decl.position, declaredName, decl.id);
            continue;
        }
        claimed.insert(realizedId);

        // Pin enforcement for a realized name.
        const QString pin = (!decl.outputId.isEmpty() && m_map.knowsScreen(decl.outputId)) ? decl.outputId : QString();
        if (!pin.isEmpty() && m_map.ownerOf(realizedId) != pin) {
            const QString previousOwner = m_map.ownerOf(realizedId);
            if (m_map.sliceSize(previousOwner) <= 1) {
                // Pinning away a screen's ONLY desktop would empty its slice,
                // and at the cap maintainScreen cannot refill it. Leave the
                // workspace where it is; a later apply retries once that
                // screen has a second desktop. Said once per deferral.
                if (!m_pinDeferredLogged.contains(realizedId)) {
                    m_pinDeferredLogged.insert(realizedId);
                    qCWarning(lcWorkspaceRec) << "not pinning" << declaredName << "to" << pin << "yet:" << previousOwner
                                              << "would be left with no workspace";
                }
            } else {
                m_pinDeferredLogged.remove(realizedId);
                m_map.transfer(realizedId, pin, namedSliceIndex(pin, decl.position));
                // A pin is a deliberate placement and outranks hotplug memory,
                // the same way the user-driven transfer verb does: leaving the
                // home stamp on would yank the workspace off its pinned output
                // the next time the stamped one is replugged.
                m_map.setHomeScreen(realizedId, QString());
                // A pin can move the desktop the previous owner was SHOWING.
                // That screen now sits on a foreign desktop and nothing else
                // would notice — the verbs' own transfer path snaps back for
                // exactly this reason.
                evaluateForeign(previousOwner);
                changed = true;
            }
        } else {
            m_pinDeferredLogged.remove(realizedId);
        }

        // Position, collected for the per-screen layout below. Only an
        // explicit position is enforced: an unpositioned workspace stays
        // where it was created, or every new workspace ahead of it would drag
        // it along. Never on a foster screen: a pinned workspace whose output
        // is unplugged, or one displaced by hotplug, sits where it does only
        // until its own output returns.
        const QString owner = m_map.ownerOf(realizedId);
        const bool onItsOwnScreen = decl.outputId.isEmpty() || owner == decl.outputId;
        if (decl.position >= 0 && !owner.isEmpty() && onItsOwnScreen
            && m_map.entryFor(realizedId).homeScreenId.isEmpty()) {
            positionedByScreen[owner].append(Positioned{realizedId, decl.position, row});
        }
    }

    // Position enforcement, laid out once per screen. Each positioned
    // workspace is placed in (position, row) order at its declared slot, but
    // never before one already placed: equal positions keep their row order,
    // and the result does not depend on where anything started, so a second
    // apply finds nothing to move. The trailing empty stays last.
    for (auto it = positionedByScreen.begin(); it != positionedByScreen.end(); ++it) {
        QList<Positioned>& placed = it.value();
        std::sort(placed.begin(), placed.end(), [](const Positioned& a, const Positioned& b) {
            return a.position != b.position ? a.position < b.position : a.row < b.row;
        });
        const QList<WorkspaceEntry> before = m_map.slice(it.key());
        QSet<QString> positionedIds;
        for (const Positioned& entry : std::as_const(placed)) {
            positionedIds.insert(entry.desktopId);
        }
        QStringList order;
        for (const WorkspaceEntry& entry : before) {
            if (!positionedIds.contains(entry.desktopId)) {
                order.append(entry.desktopId);
            }
        }
        const QString trailing = trailingEmptyOf(it.key());
        const bool keepTrailingLast = !trailing.isEmpty() && !order.isEmpty() && order.last() == trailing;
        if (keepTrailingLast) {
            order.removeLast();
        }
        int previous = -1;
        for (const Positioned& entry : std::as_const(placed)) {
            const int slot = qBound(0, qMax(entry.position, previous + 1), static_cast<int>(order.size()));
            order.insert(slot, entry.desktopId);
            previous = slot;
        }
        if (keepTrailingLast) {
            order.append(trailing);
        }
        if (m_map.reorderSlice(it.key(), order)) {
            changed = true;
        }
    }

    // Pass 2: a named entry no declaration claimed reverts to dynamic. That
    // covers a removed declaration, and a second entry carrying the same name
    // (two slices restored with it), which would otherwise stay destroy-exempt
    // with nothing in settings to explain it.
    const QStringList owned = m_map.allDesktopIds();
    for (const QString& id : owned) {
        const WorkspaceEntry entry = m_map.entryFor(id);
        if (claimed.contains(id) || (entry.name.isEmpty() && entry.namedId.isEmpty())) {
            continue;
        }
        m_map.setName(id, QString());
        m_map.setNamedId(id, QString());
        m_namePushes.remove(id);
        m_reportedKWinRenames.remove(id);
        m_pinDeferredLogged.remove(id);
        changed = true;
        if (entry.name.isEmpty()) {
            continue;
        }
        // KWin keeps whatever name it is given, so an empty push would leave
        // a blank label in its UI and its config. Unnamed means KWin's own
        // positional default, which is also what the label refresh keeps
        // current from here on.
        const int position = static_cast<int>(m_lastIds.indexOf(id));
        if (position >= 0) {
            pushDesktopName(id, defaultDesktopName(position + 1));
        }
    }

    // A reverted entry that is now surplus is reaped by maintenance, whose
    // surplus scan re-arms the destroy check for every non-trailing empty.
    if (changed) {
        maintainInvariants();
        bumpGeneration();
    }
}

} // namespace PhosphorWorkspaces
