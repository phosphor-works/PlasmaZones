// SPDX-FileCopyrightText: 2026 fuddlesworth
// SPDX-License-Identifier: LGPL-2.1-or-later

#pragma once

#include <PhosphorEngine/DesktopRemap.h>
#include <PhosphorEngine/IVirtualDesktopManager.h>
#include "phosphorworkspaces_export.h"
#include <QHash>
#include <QObject>
#include <QString>
#include <QStringList>

QT_BEGIN_NAMESPACE
class QDBusArgument;
class QDBusInterface;
class QDBusMessage;
QT_END_NAMESPACE

namespace PhosphorWorkspaces {

class PHOSPHORWORKSPACES_EXPORT VirtualDesktopManager : public QObject, public PhosphorEngine::IVirtualDesktopManager
{
    Q_OBJECT

public:
    /// How KWin answered one of the desktop methods (createDesktopAnswered,
    /// removeDesktopAnswered).
    enum class CallAnswer {
        /// KWin returned from the method. It emits the desktop signals of
        /// whatever the method did before it returns, so by now every echo of
        /// it has arrived: no echo means KWin refused (at its desktop cap, the
        /// last desktop, an unknown id).
        Answered,
        /// An error came back instead, a timeout included (the interface's
        /// one-second bound). A stall, not a refusal: KWin may still run the
        /// method and its echo may still land.
        Unanswered,
    };
    Q_ENUM(CallAnswer)

    explicit VirtualDesktopManager(QObject* parent = nullptr);
    ~VirtualDesktopManager() override;

    bool init();
    void start();
    void stop();

    int currentDesktop() const override;
    int currentDesktopForScreen(const QString& screenId) const override;
    /// Superseded, kept for the installed API (IVirtualDesktopManager): nothing
    /// in this tree calls it. It only tells whether two recorded screens differ
    /// right now, which is not whether KWin runs per-output desktops (two
    /// screens on the same desktop answer false in that mode too).
    bool perScreenModeActive() const override;

    /// Record a screen's current virtual desktop (1-based). This is fed by the
    /// KWin effect's per-output desktopChanged report (Plasma 6.7 per-output
    /// virtual desktops) — KWin's own D-Bus VirtualDesktopManager interface only
    /// exposes the GLOBAL current desktop, so per-screen data arrives this way.
    /// Emits screenDesktopChanged only when the value actually changes.
    void updateScreenDesktop(const QString& screenId, int desktop);

    /// Drop a screen's recorded per-output desktop when the output is removed,
    /// so the map doesn't retain stale entries (and perScreenModeActive() doesn't
    /// keep counting a gone screen) across monitor hot-plug. Driven by the
    /// daemon's screenRemoved handler.
    void removeScreenDesktop(const QString& screenId);
    /// True when a per-output report exists for @p screenId (or, for a virtual
    /// screen id, for its parent output). currentDesktopForScreen falls back
    /// to the global current when this is false, so callers that must not act
    /// on the fallback (first-run adoption) gate on this instead.
    bool hasScreenDesktopReport(const QString& screenId) const;
    /// Carry a recorded per-output desktop across a screen-id rename. KWin can
    /// flip an output's id between its bare and "/CONNECTOR"-qualified
    /// spellings and ScreenManager reports that as screenIdentifierChanged,
    /// with no screenRemoved/screenAdded; without this the row stays under the
    /// dead id and every hasScreenDesktopReport / currentDesktopForScreen ask
    /// under the live id falls back to the GLOBAL desktop.
    ///
    /// The carried row goes out as screenDesktopSeeded, never as
    /// screenDesktopChanged: the output did not switch desktops, it was renamed,
    /// and every consumer keyed on the screen id must learn its desktop under
    /// the new id without running a desktop-switch pass.
    ///
    /// A row already present under @p newId wins and @p oldId is merely
    /// dropped: that row is a report the effect already made under the live id,
    /// so it is newer than the one carried over. Nothing is emitted in that
    /// case, nor when @p oldId is unknown or either id is empty or the two are
    /// equal.
    void renameScreen(const QString& oldId, const QString& newId);

    /// Ask KWin to switch to the desktop at this 1-based position. Asynchronous
    /// (a Properties.Set the caller does not wait for): the switch arrives as
    /// currentDesktopChanged.
    void setCurrentDesktop(int desktop);
    /// Switch by KWin's desktop UUID rather than by position. Positions
    /// renumber whenever a desktop is created or removed, so anything that
    /// holds a desktop across such an event (a pager's pills, a rule) must
    /// key on the id. Unknown ids are ignored. Asynchronous, like
    /// setCurrentDesktop.
    void setCurrentDesktopById(const QString& desktopId);
    /// Ask KWin to create a desktop at the given 0-based global position (KWin's
    /// D-Bus signature; the caller computes the position, this wrapper only
    /// forwards). The desktop arrives as kwinDesktopCreated and then the
    /// committed list (desktopListChanged), and KWin's answer to the call as
    /// createDesktopAnswered after both. Dropped without an answer when KWin's
    /// interface is unavailable.
    void createDesktop(uint position, const QString& name);
    /// Ask KWin to remove the desktop with the given UUID string. Result arrives
    /// as kwinDesktopRemoved + desktopListChanged, then removeDesktopAnswered.
    void removeDesktop(const QString& desktopId);
    /// Ask KWin to rename the desktop with the given UUID string. The new name
    /// arrives as desktopRenamed. KWin's reply is not reported: an unknown id is
    /// ignored by KWin, and nothing here keeps a ledger of renames.
    void setDesktopName(const QString& desktopId, const QString& name);
    int desktopCount() const;
    /// Number of rows in KWin's virtual-desktop grid (>= 1). With the count,
    /// this gives the grid shape that cross-desktop directional navigation
    /// walks. Defaults to 1 until the first KWin refresh.
    int desktopRows() const;
    QStringList desktopNames() const;
    /// KWin's desktop UUIDs, in position order and index-aligned with
    /// desktopNames(). The stable identity of a desktop: names are
    /// user-editable and positions renumber, ids do neither.
    QStringList desktopIds() const;
    /// UUID at 1-based global index, or empty when out of range.
    QString desktopIdAt(int desktop) const;
    /// 1-based global index of the UUID, or 0 when unknown.
    int desktopIndexOf(const QString& desktopId) const;
    /// KWin's names EXACTLY as KWin reports them, aligned 1:1 with
    /// desktopIds(). KWin gives every desktop it creates without a name a
    /// positional default label of its own ("Desktop N", in KWin's language),
    /// so an entry is empty only when a client explicitly set an empty name;
    /// it is never a reliable "unnamed" marker. desktopNames() differs only
    /// there, filling the empty entries with this library's placeholder. Empty
    /// without KWin.
    QStringList rawDesktopNames() const;
    /// True once KWin's VirtualDesktopManager interface answered on the
    /// session bus. False on a non-KWin compositor or before init(), where
    /// every getter degrades to a single synthetic desktop — a UI should
    /// hide itself rather than show a pager with one permanent pill.
    /// init() returns true unconditionally and so cannot be used for this.
    bool isAvailable() const;

    /// Record the desktop an output shows as it is connected or comes back.
    /// KWin picks it without a desktop change, so it is a seed, not a switch:
    /// it emits screenDesktopSeeded, never screenDesktopChanged, and always,
    /// because the engines dropped the output's desktop when it went away.
    void seedScreenDesktop(const QString& screenId, int desktop);

    /// The desktop ids a previous session saw, in position order, so a change
    /// KWin made while nothing was listening (desktops added, removed or moved
    /// while the daemon was down) still renumbers the stores keyed on the old
    /// positions. Compared against the first committed snapshot, or against
    /// the current one at once when it has already landed; a difference goes
    /// out as desktopsRenumbered and nothing else (the per-screen desktops and
    /// desktopRemovedAt are about live changes only). An empty list is ignored.
    void setBaselineDesktopIds(const QStringList& desktopIds);

Q_SIGNALS:
    void currentDesktopChanged(int desktop);
    /// The committed count changed. For a create or a remove it goes out with
    /// the list, last of that commit's signals. Only when KWin's signals for the
    /// change did not add up to a list (a desktop signal missed) is a GROWING
    /// count committed as KWin reports it, ahead of the list (F1022), and then
    /// desktopIds() and desktopNames() still hold the old list here;
    /// desktopsChanged follows once the list commits, and is the signal a
    /// consumer of the names listens to (F1101).
    void desktopCountChanged(int count);
    /// The desktop LIST changed — a desktop was added, removed, reordered,
    /// or renamed. Distinct from desktopCountChanged, which a rename does
    /// not move: KWin delivers renames through `desktopDataChanged`, and
    /// without this signal a pager would keep showing the old label until
    /// something unrelated forced a refresh. Emitted only when the ids or
    /// names actually differ from the previous snapshot.
    void desktopsChanged();
    /// Exactly one desktop was removed, and this is the 1-based POSITION it
    /// held before it went. Emitted ahead of desktopCountChanged, because a
    /// consumer keyed on positions has to act in that order: drop the removed
    /// position's state first, then shift everything above it down one.
    ///
    /// desktopCountChanged cannot carry this. It reports the new count, and
    /// after a mid-list removal every surviving position is still within it —
    /// so a consumer seeing only the count cannot tell 2-of-4 was deleted from
    /// 4-of-4, and pruning "everything past the new count" drops the TOPMOST
    /// desktop's state instead of the removed one's.
    ///
    /// Only for the unambiguous case: exactly one id gone and none added. A
    /// reorder, a simultaneous add and remove, or a multi-desktop change
    /// leaves this silent.
    ///
    /// Superseded by desktopsRenumbered, which goes out just before it and
    /// covers every shape of change. Nothing in this tree connects it; it is
    /// kept for installed-API compatibility. Connect one of the two, never
    /// both, or a store is shifted twice.
    void desktopRemovedAt(int removedPosition);
    /// A single screen's current virtual desktop changed (per-output virtual
    /// desktops). The primary trigger the daemon's per-screen desktop handler
    /// subscribes to; in single-desktop mode it is driven the same for every
    /// screen so downstream has one code path.
    void screenDesktopChanged(const QString& screenId, int desktop);
    /// A screen's desktop was recorded as the output (re)connected
    /// (seedScreenDesktop). Not a switch.
    void screenDesktopSeeded(const QString& screenId, int desktop);
    /// The desktop positions moved: a removal, an insert, a reorder, or any
    /// mix of them, as one old-to-new map (F787). Every store keyed on a
    /// desktop NUMBER applies @p remap once. By the time it goes out the
    /// manager's own state is renumbered: the ids, the count, the current
    /// desktop, and each screen's desktop (moved through
    /// DesktopRemap::successor, silently, for the reason desktopRemovedAt's
    /// shift is silent). Ahead of desktopCountChanged, like desktopRemovedAt.
    /// Only in the fallback desktopCountChanged describes (a growing count
    /// committed before its list) does the count go first. Never sent for a
    /// change that moves no number (a rename, a desktop appended after the
    /// last one).
    void desktopsRenumbered(const PhosphorEngine::DesktopRemap& remap);
    /// KWin started a change that can move desktop positions (a create, a
    /// remove, a move), and the list that says what changed is on its way.
    /// Sent once per change, on the first of the several signals KWin sends
    /// for it (F1023), and paired with desktopListChangeSettled. A rename moves
    /// no position and sends neither. Not sent before the first snapshot, whose
    /// map compares against a previous session. A change that moves no number
    /// is never followed by desktopsRenumbered. A store whose entries the
    /// compositor re-reports in the new numbering on its own (the window
    /// registry) marks here which entries predate the change.
    void desktopListChanging();
    /// The change desktopListChanging announced has committed, or was given up
    /// on after the list could not be read. Sent after every other signal of
    /// that commit, desktopsRenumbered included when there was one, so a store
    /// that marked its entries on desktopListChanging drops the marks here
    /// whether or not anything was renumbered.
    void desktopListChangeSettled();
    /// A desktop present before and after a commit carries a different raw
    /// KWin name (rawDesktopNames). Sent for every such rename, those this
    /// process asked for with setDesktopName included, after desktopsChanged.
    void desktopRenamed(const QString& desktopId, const QString& name);
    /// KWin answered the createDesktop call made with these arguments, after
    /// any kwinDesktopCreated it caused (CallAnswer). One per call made while
    /// KWin's interface was available.
    void createDesktopAnswered(uint position, const QString& name,
                               PhosphorWorkspaces::VirtualDesktopManager::CallAnswer answer);
    /// KWin answered the removeDesktop call for @p desktopId, after any
    /// kwinDesktopRemoved it caused (CallAnswer).
    void removeDesktopAnswered(const QString& desktopId, PhosphorWorkspaces::VirtualDesktopManager::CallAnswer answer);
    /// A KWin desktopCreated arrived carrying this UUID. Emitted BEFORE the
    /// list commits: id-only, desktopIds() still the old list. The
    /// dynamic-workspaces reconciler matches its ledger on this, then acts on
    /// desktopListChanged.
    void kwinDesktopCreated(const QString& desktopId);
    /// A KWin desktopRemoved arrived carrying this UUID (same timing contract).
    void kwinDesktopRemoved(const QString& desktopId);
    /// The ordered global id list settled (create, remove, reorder). Emitted
    /// only when the ordered id list actually changed, and BEFORE any
    /// screenDesktopChanged or desktopCountChanged the same commit drives, so a
    /// consumer resolving those numbers against the id list it last settled
    /// reads them against the new list. The per-screen desktops
    /// (currentDesktopForScreen) are already in the new numbering here.
    ///
    /// A create or a remove commits when KWin's countChanged closes it, built
    /// from the desktop signals KWin sent for it, so this goes out before any
    /// report KWin sent after the change. A report KWin sent DURING it (the
    /// active output's move off a removed desktop) still arrives first. A move
    /// or a rename commits with the next list read.
    void desktopListChanged(const QStringList& desktopIds);

protected:
    /// One desktop of a KWin list: 0-based position, UUID, raw name.
    struct DesktopEntry
    {
        int position = 0;
        QString id;
        QString name;
    };
    /// Commit one whole KWin desktop list: the core behind the blocking Get,
    /// the asynchronous GetAll, and the list built from KWin's desktop signals
    /// at countChanged. Returns false, committing nothing, for a list KWin can
    /// never send (empty, an empty id, a duplicate id). Protected so a driver
    /// with no compositor (the library's tests) can feed lists directly.
    bool commitDesktopList(QList<DesktopEntry> desktops, const QString& currentId, int rows);
    /// KWin's desktopCreated and desktopDataChanged, parsed out of the D-Bus
    /// message. The other KWin signals carry plain values and their slots take
    /// them directly.
    void ingestDesktopCreated(const QString& desktopId, int position, const QString& name);
    void ingestDesktopDataChanged(const QString& desktopId, int position, const QString& name);

private Q_SLOTS:
    /// KWin's countChanged carries `u`, not `i`. A slot declared `int`
    /// registers successfully and is then NEVER invoked, because each
    /// argument's type must match the message's.
    void onNumberOfDesktopsChanged(uint count);
    void refreshFromKWin();
    void onKWinCurrentChanged(const QString& desktopId);
    /// desktopCreated is s(iss); the whole message is taken so the struct can
    /// be read (ingestDesktopCreated).
    void onKWinDesktopCreated(const QDBusMessage& message);
    void onKWinDesktopRemoved(const QString& desktopId);
    /// rowsChanged carries the new row count (`u`).
    void onKWinDesktopRowsChanged(uint rows);
    /// KWin's per-desktop metadata changed (a rename, or a position move), as
    /// s(iss) (ingestDesktopDataChanged).
    void onKWinDesktopDataChanged(const QDBusMessage& message);

private:
    void initKWinDBus();
    /// Install the session-bus signal subscriptions. Idempotent via
    /// m_kwinSubscribed, so a stop()/start() cycle re-subscribes without
    /// ever doubling the hooks.
    void subscribeToKWin();
    void applyDesktopListReply(const QDBusMessage& reply, const QString& currentId, int rows);
    /// Parse the a(uss) desktop array and commit it (commitDesktopList).
    /// Returns false when the array is malformed or the commit refused it.
    bool applyDesktopListArg(const QDBusArgument& arg, const QString& currentId, int rows);
    /// Clamp any per-screen desktop entry above the live desktop count back down
    /// to the count. Only the list-less count fallback leaves anything for it.
    void clampScreenDesktopsToCount();
    /// Emit desktopListChanging unless this change already did.
    void noteDesktopListChanging();
    /// A list read failed: try again a bounded number of times, then give the
    /// change up (roll an uncommitted count back to the committed list).
    void retryOrAbandonRefresh();
    /// The list built from KWin's signals is a whole, consistent list of
    /// @p count desktops (onNumberOfDesktopsChanged commits it).
    bool liveListMatches(int count) const;
    /// Commit the list built from KWin's signals since the last commit.
    void commitLiveList();

    QDBusInterface* m_kwinVDInterface = nullptr;
    bool m_running = false;
    /// True while the session-bus subscriptions are installed; guards
    /// subscribeToKWin against doubling them.
    bool m_kwinSubscribed = false;
    bool m_useKWinDBus = false;
    int m_currentDesktop = 1;
    /// Per-screen current virtual desktop (screenId → 1-based), populated only
    /// in per-output mode via updateScreenDesktop. Empty otherwise, so every
    /// currentDesktopForScreen() falls back to the global m_currentDesktop.
    QHash<QString, int> m_screenDesktops;
    int m_desktopCount = 1;
    int m_desktopRows = 1;
    QStringList m_desktopNames;
    QStringList m_desktopIds;
    uint m_refreshGeneration = 0;
    /// setBaselineDesktopIds' list, held until the first snapshot commits.
    QStringList m_baselineDesktopIds;
    /// desktopListChanging went out and no snapshot has committed since
    /// (noteDesktopListChanging). Appended last: installed class.
    bool m_desktopListChangePending = false;
    /// KWin's names as reported, empty entries included (rawDesktopNames).
    /// Appended last, like every member below it.
    QStringList m_rawDesktopNames;
    /// The desktop list as KWin's desktop signals since the last commit say it
    /// is, in bus order: commitLiveList's input. Equal to m_desktopIds after
    /// every commit, empty before the first.
    QStringList m_liveIds;
    /// Raw names for m_liveIds, from the same signals.
    QHash<QString, QString> m_liveRawNames;
    /// A currentChanged that arrived while a change was pending: its id is
    /// numbered by the commit that ends the change (F950).
    QString m_pendingCurrentId;
    /// A GetAll is in flight. Its reply describes KWin after every KWin
    /// signal received before it, so no second read is issued meanwhile (F914).
    bool m_refreshInFlight = false;
    /// Failed list reads in a row (retryOrAbandonRefresh).
    int m_refreshRetries = 0;
};

} // namespace PhosphorWorkspaces
