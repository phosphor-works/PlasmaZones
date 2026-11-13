// SPDX-FileCopyrightText: 2026 fuddlesworth
// SPDX-License-Identifier: LGPL-2.1-or-later

#pragma once

// Shared fixtures for the WorkspaceMap and WorkspaceReconciler suites. The
// reconciler's inputs are plain calls, so a "world" is just a scripted
// sequence of them. These are the starting points most cases share; a case
// that needs a different shape builds its own. Split into a header because the
// suites are split by concern (map value type, structural behaviour, and the
// timing-driven ledger paths).
//
// Nothing here plays KWin on its own: a case answers each requestCreateDesktop
// itself, through onKwinDesktopCreated and onDesktopListSettled, at the moment
// the case is about.
//
// The helpers return false on a broken precondition instead of failing inside
// a void function, which QCOMPARE would only report while letting the calling
// case carry on against a world it never got. Callers wrap them in QVERIFY.

#include <PhosphorWorkspaces/WorkspaceReconciler.h>

#include <QHash>
#include <QSignalSpy>
#include <QString>
#include <QTest>

namespace WorkspaceTest {

/// A desktop id in the shape KWin mints: an unbraced UUID.
inline QString id(int n)
{
    return QStringLiteral("00000000-0000-4000-8000-%1").arg(n, 12, 10, QLatin1Char('0'));
}

/// Shortened ledger expiry and destroy debounce for the timing-driven cases,
/// so they wait a fraction of the production timeouts.
constexpr int FastLedgerMs = 200;
constexpr int FastDebounceMs = 30;

/// Two screens, each occupied and each therefore owing a trailing-empty
/// create, with BOTH Creates open in the ledger at once. A owns {d1}
/// (occupied), B owns {d2} (occupied); nothing has landed yet.
/// `populateBFirst` flips which screen's population change fires first, and
/// so which Create heads the ledger. The screen ORDER is [A,B] either way,
/// so B-first is the case where oldest-request order and KWin-position
/// order disagree.
inline bool openTwoConcurrentCreates(PhosphorWorkspaces::WorkspaceReconciler& rec, QSignalSpy& createSpy,
                                     bool populateBFirst = false)
{
    rec.onScreenOrderChanged({QStringLiteral("A"), QStringLiteral("B")});
    rec.setFocusedScreen(QStringLiteral("A"));

    QHash<QString, QString> current;
    current.insert(QStringLiteral("A"), id(1));
    current.insert(QStringLiteral("B"), id(2));
    if (!rec.adoptAll({id(1), id(2)}, current) || createSpy.count() != 0) {
        return false;
    }

    if (populateBFirst) {
        rec.onPopulationChanged(id(2), 1);
        rec.onPopulationChanged(id(1), 1);
    } else {
        rec.onPopulationChanged(id(1), 1);
        rec.onPopulationChanged(id(2), 1);
    }
    return createSpy.count() == 2;
}

/// Adopt a two-screen world: A owns {d1} (current), B owns {d2} (current).
/// With an empty census both slices already end in an empty dynamic
/// desktop, so adoption alone requests nothing; occupying each current
/// desktop then drives the trailing-empty creates, answered so A has
/// {d1(occupied), d3(empty)} and B has {d2(occupied), d4(empty)}. The
/// settled list is {d1, d3, d2, d4}, so the 1-based positions are d1=1,
/// d3=2, d2=3, d4=4.
inline bool adoptTwoScreens(PhosphorWorkspaces::WorkspaceReconciler& rec)
{
    rec.onScreenOrderChanged({QStringLiteral("A"), QStringLiteral("B")});
    rec.setFocusedScreen(QStringLiteral("A"));

    QSignalSpy createSpy(&rec, &PhosphorWorkspaces::WorkspaceReconciler::requestCreateDesktop);
    QHash<QString, QString> current;
    current.insert(QStringLiteral("A"), id(1));
    current.insert(QStringLiteral("B"), id(2));
    if (!rec.adoptAll({id(1), id(2)}, current) || createSpy.count() != 0) {
        return false; // both slices end in an empty desktop
    }

    rec.onPopulationChanged(id(1), 1);
    rec.onPopulationChanged(id(2), 1);
    if (createSpy.count() != 2) {
        return false;
    }
    // Answer them: A's lands as {d3} at global position 1, B's as {d4}.
    rec.onKwinDesktopCreated(id(3));
    rec.onKwinDesktopCreated(id(4));
    rec.onDesktopListSettled({id(1), id(3), id(2), id(4)});

    return rec.map().slice(QStringLiteral("A")).size() == 2 && rec.map().slice(QStringLiteral("B")).size() == 2
        && rec.map().ownerOf(id(3)) == QStringLiteral("A") && rec.map().ownerOf(id(4)) == QStringLiteral("B")
        && !rec.hasPendingStructuralOps();
}

/// The desktop ids of a screen's slice, in slice order.
inline QStringList sliceIds(const PhosphorWorkspaces::WorkspaceReconciler& rec, const QString& screenId)
{
    QStringList ids;
    const auto entries = rec.map().slice(screenId);
    for (const auto& entry : entries) {
        ids.append(entry.desktopId);
    }
    return ids;
}

} // namespace WorkspaceTest
