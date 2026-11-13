// SPDX-FileCopyrightText: 2026 fuddlesworth
// SPDX-License-Identifier: LGPL-2.1-or-later

#include <PhosphorWorkspaces/WorkspaceReconciler.h>

#include <PhosphorEngine/DesktopRemap.h>

#include <QLoggingCategory>
#include <QSet>

#include <algorithm>
#include <iterator>
#include <utility>

Q_LOGGING_CATEGORY(lcWorkspaceRec, "org.phosphor.workspaces.reconciler", QtInfoMsg)

namespace PhosphorWorkspaces {

WorkspaceReconciler::WorkspaceReconciler(QObject* parent)
    : QObject(parent)
{
    // Monotonic: every deadline here is a duration measured from now, and a
    // wall-clock step (NTP, suspend, a manual change) would expire the whole
    // ledger at once or hold it open for as long as the clock went back.
    m_clock.start();
    m_ledgerTimer.setInterval(qMax(1, m_ledgerTimeoutMs / 4));
    m_ledgerTimer.setSingleShot(false);
    connect(&m_ledgerTimer, &QTimer::timeout, this, &WorkspaceReconciler::expireLedger);
}

const WorkspaceMap& WorkspaceReconciler::map() const
{
    return m_map;
}

quint64 WorkspaceReconciler::generation() const
{
    return m_generation;
}

bool WorkspaceReconciler::restoreState(const QString& json,
                                       const std::function<QString(const QString&)>& canonicalScreenId)
{
    // The candidate is only meaningful before adoption: afterwards the map is
    // the reconciled truth, and replacing it would discard ownership the
    // ledger and the engines are already acting on.
    if (m_adopted) {
        qCWarning(lcWorkspaceRec) << "ignoring a workspace state restore after adoption";
        return false;
    }
    WorkspaceMap parsed;
    if (!parsed.fromJson(json)) {
        return false;
    }
    if (canonicalScreenId) {
        const QStringList ids = parsed.allDesktopIds();
        for (const QString& id : ids) {
            const QString home = parsed.entryFor(id).homeScreenId;
            if (home.isEmpty()) {
                continue;
            }
            const QString canonical = canonicalScreenId(home);
            // A home that resolves to the entry's own owner is no
            // displacement at all.
            parsed.setHomeScreen(id, canonical == parsed.ownerOf(id) ? QString() : canonical);
        }
    }
    m_map = parsed;
    bumpGeneration();
    return true;
}

bool WorkspaceReconciler::isAdopted() const
{
    return m_adopted;
}

int WorkspaceReconciler::desktopCap() const
{
    return m_desktopCap;
}

void WorkspaceReconciler::setDesktopCap(int cap)
{
    m_configuredCap = qMax(1, cap);
    m_desktopCap = m_configuredCap;
}

void WorkspaceReconciler::setTimeouts(int ledgerTimeoutMs, int destroyDebounceMs)
{
    m_ledgerTimeoutMs = qMax(1, ledgerTimeoutMs);
    m_destroyDebounceMs = qMax(1, destroyDebounceMs);
    m_ledgerTimer.setInterval(qMax(1, m_ledgerTimeoutMs / 4));
}

int WorkspaceReconciler::ledgerTimeoutMs() const
{
    return m_ledgerTimeoutMs;
}

int WorkspaceReconciler::destroyDebounceMs() const
{
    return m_destroyDebounceMs;
}

void WorkspaceReconciler::setFocusedScreen(const QString& screenId)
{
    if (!screenId.isEmpty()) {
        m_focusedScreen = screenId;
    }
}

void WorkspaceReconciler::setFocusedScreenProvider(std::function<QString()> provider)
{
    m_focusedScreenProvider = std::move(provider);
}

void WorkspaceReconciler::setDefaultDesktopNameProvider(std::function<QString(int)> provider)
{
    m_defaultNameProvider = std::move(provider);
}

void WorkspaceReconciler::bumpGeneration()
{
    ++m_generation;
    Q_EMIT mapChanged();
}

qint64 WorkspaceReconciler::nowMs() const
{
    return m_clock.elapsed();
}

QString WorkspaceReconciler::defaultDesktopName(int oneBasedPosition) const
{
    if (m_defaultNameProvider) {
        return m_defaultNameProvider(oneBasedPosition);
    }
    return QStringLiteral("Desktop %1").arg(oneBasedPosition);
}

bool WorkspaceReconciler::isPositionalLabel(const QString& name) const
{
    if (name.isEmpty()) {
        return false;
    }
    // Any position, not only the desktop's own: a label goes stale exactly
    // when a create or remove ahead of the desktop shifts it.
    const int limit = qMax(qMax(m_desktopCap, m_configuredCap), static_cast<int>(m_lastIds.size()) + 1);
    for (int position = 1; position <= limit; ++position) {
        if (name == defaultDesktopName(position)) {
            return true;
        }
    }
    return false;
}

bool WorkspaceReconciler::pushDesktopName(const QString& desktopId, const QString& name)
{
    // Once per push window: applyNamedWorkspaces and the label refresh run on
    // every settle, and a KWin that declines the rename (or whose name list
    // simply lags the reply) would otherwise see the identical push re-fired
    // forever. The deadline is what lets a genuinely lost push be retried, and
    // the refusal count bounds the retries.
    const qint64 now = nowMs();
    const auto push = m_namePushes.constFind(desktopId);
    const bool sameName = push != m_namePushes.constEnd() && push->name == name;
    if (sameName && push->deadline > now) {
        return false;
    }
    const int refusals = sameName ? push->refusals + 1 : 0;
    if (refusals >= MaxNamePushRefusals) {
        // Said once, when the budget runs out, not on every later settle.
        if (push->refusals < MaxNamePushRefusals) {
            qCWarning(lcWorkspaceRec) << "renaming" << desktopId << "to" << name << "went unanswered" << refusals
                                      << "times, leaving KWin's name alone";
            m_namePushes.insert(desktopId, NamePush{name, 0, MaxNamePushRefusals});
        }
        return false;
    }
    m_namePushes.insert(desktopId, NamePush{name, now + m_ledgerTimeoutMs, refusals});
    Q_EMIT requestSetDesktopName(desktopId, name);
    return true;
}

void WorkspaceReconciler::refreshPositionalLabels()
{
    // KWin names a desktop after its position once, at creation, and never
    // again, so a create or remove ahead of it leaves "Desktop 4" sitting on
    // the third desktop. Only a label that is positional (or empty, which an
    // older revert pushed) is ours to correct: a name a user gave a dynamic
    // desktop elsewhere stays exactly as it is.
    if (!m_adopted || m_lastRawNames.size() != m_lastIds.size()) {
        return;
    }
    for (int i = 0; i < m_lastIds.size(); ++i) {
        const QString& desktopId = m_lastIds.at(i);
        const WorkspaceEntry entry = m_map.entryFor(desktopId);
        if (entry.desktopId.isEmpty() || !entry.name.isEmpty()) {
            continue;
        }
        const QString& raw = m_lastRawNames.at(i);
        const QString expected = defaultDesktopName(i + 1);
        if (raw == expected) {
            m_namePushes.remove(desktopId);
            continue;
        }
        if (!raw.isEmpty() && !isPositionalLabel(raw)) {
            continue;
        }
        pushDesktopName(desktopId, expected);
    }
}

// ── Ledger ──────────────────────────────────────────────────────────────────

void WorkspaceReconciler::ledgerAdd(PendingOp op)
{
    op.deadline = nowMs() + m_ledgerTimeoutMs;
    m_ledger.append(op);
    if (!m_ledgerTimer.isActive()) {
        m_ledgerTimer.start();
    }
}

void WorkspaceReconciler::stopLedgerTimerIfIdle()
{
    if (m_ledger.isEmpty()) {
        m_ledgerTimer.stop();
    }
}

void WorkspaceReconciler::announceIfQuiet(bool wasPending)
{
    if (wasPending && !hasPendingStructuralOps()) {
        Q_EMIT structuralOpsSettled();
    }
}

void WorkspaceReconciler::expireLedger()
{
    const bool wasPending = hasPendingStructuralOps();
    const WorkspaceMap before = m_map;
    const qint64 now = nowMs();
    bool expired = false;
    bool capEvidence = false;
    bool createExpired = false;
    bool namedCreateExpired = false;
    QStringList refusedRemovals;
    QSet<QString> refusedCreateScreens;
    QSet<QString> refusedNamedKeys;
    QSet<QString> refusedSetCurrentScreens;
    for (int i = m_ledger.size() - 1; i >= 0; --i) {
        const PendingOp op = m_ledger.at(i);
        if (op.deadline > now) {
            continue;
        }
        expired = true;
        m_ledger.removeAt(i);
        qCWarning(lcWorkspaceRec) << "pending workspace op expired, kind =" << static_cast<int>(op.kind)
                                  << "desktop =" << op.desktopId << "echoed =" << op.echoed;
        if (op.echoed) {
            // KWin answered; only the settled list confirming it is late. No
            // refusal to count and no cap evidence: the resync below re-pulls
            // the list that closes it.
            continue;
        }
        switch (op.kind) {
        case PendingOp::Kind::Create:
            // A create whose call failed or timed out is a stall, and a stall
            // says nothing about KWin's ceiling.
            capEvidence = capEvidence || !op.stalled;
            createExpired = true;
            // One refusal per screen per pass, and only for unnamed creates:
            // several creates expiring together are one stall, not several,
            // and a named create spends its declaration's own budget.
            if (op.name.isEmpty()) {
                refusedCreateScreens.insert(op.screenId);
            } else {
                refusedNamedKeys.insert(op.namedId.isEmpty() ? op.name : op.namedId);
                namedCreateExpired = true;
            }
            break;
        case PendingOp::Kind::Remove:
            refusedRemovals.append(op.desktopId);
            break;
        case PendingOp::Kind::SetCurrent:
            refusedSetCurrentScreens.insert(op.screenId);
            break;
        }
    }
    stopLedgerTimerIfIdle();
    // The timer ticks several times per timeout. A tick that retired nothing
    // changed nothing, and running the sweeps below on it would re-raise
    // every standing foreign screen once a tick.
    if (!expired) {
        return;
    }

    // Live cap probe: KWin refuses createDesktop past its maximum SILENTLY (a
    // void method, no desktopCreated echo), so a Create expiry with the id list
    // unchanged is the only observable form of the refusal.
    //
    // One expiry is NOT that evidence: a D-Bus stall, a KWin restart or a lost
    // reply also expires a Create with the list unchanged, and lowering the
    // ceiling then suspends every later create, leaving the daemon unable to
    // produce the successful create that would disprove it. A genuine refusal
    // answers EVERY attempt the same way, so the probe asks twice: the first
    // expiry only records the episode and re-drives maintenance, and only a
    // second expiry in the same episode (the id list byte-identical, so
    // nothing landed in between) is read as the real ceiling.
    // noteCreateSucceeded() clears the evidence the moment KWin answers a
    // create at all.
    if (capEvidence && !m_lastIds.isEmpty() && knownDesktopCount() < m_desktopCap) {
        if (m_capProbeIds != m_lastIds) {
            m_capProbeIds = m_lastIds;
            m_capProbeExpiries = 1;
        } else {
            ++m_capProbeExpiries;
        }
        if (m_capProbeExpiries >= CapProbeExpiries) {
            m_desktopCap = knownDesktopCount();
            qCWarning(lcWorkspaceRec) << "createDesktop went unanswered" << m_capProbeExpiries << "times at"
                                      << m_desktopCap << "desktops, learned as the compositor's cap";
            if (!m_capHintShown) {
                m_capHintShown = true;
                Q_EMIT capReached();
            }
        }
    }

    // A removal KWin refused (or lost) leaves a surplus empty desktop that
    // nothing would ever retry: the destroy timer was consumed when the Remove
    // was issued, and the population gate only fires on a fresh >0 → 0 edge
    // that will never come for a desktop that is already empty. Re-arm the
    // destroy check so maintenance gets another attempt.
    //
    // Bounded, though: a removal KWin genuinely REFUSES answers every re-arm
    // the same way, and an unbounded retry is a re-arm/re-issue/expire/resync
    // loop that never settles. After MaxRemovalRefusals rounds the surplus
    // empty desktop is left in place until its population changes or it goes
    // away, either of which restores the budget.
    for (const QString& desktopId : std::as_const(refusedRemovals)) {
        m_racedDesktops.remove(desktopId);
        Q_EMIT removalAbandoned(desktopId);
        const int refusals = m_removalRefusals.value(desktopId, 0) + 1;
        m_removalRefusals.insert(desktopId, refusals);
        if (refusals >= MaxRemovalRefusals) {
            if (refusals == MaxRemovalRefusals) {
                qCWarning(lcWorkspaceRec) << "removal of" << desktopId << "went unanswered" << refusals
                                          << "times, leaving the desktop in place";
            }
            continue;
        }
        if (isReapable(desktopId)) {
            scheduleDestroyCheck(desktopId);
        }
    }

    // A Create nobody answered leaves the invariant it was repairing (a missing
    // trailing empty, a screen with no desktop) still broken, and nothing else
    // re-drives it: maintenance runs off settles and population edges, and the
    // resync below re-pulls a list that is by definition UNCHANGED, so
    // onDesktopListSettled returns early. Re-run maintenance so the request is
    // made again. That second attempt is also what gives the probe above its
    // corroborating expiry.
    //
    // Bounded by its OWN budget, not by cap-learning: the probe is reset by
    // any list change and by ANY create landing, so on a machine where this
    // screen's creates are refused while other desktops come and go it never
    // concludes. requestCreateAt refuses once the budget is gone, so the
    // re-drive becomes a no-op for that screen while a transient stall still
    // gets its retries.
    for (const QString& screenId : std::as_const(refusedCreateScreens)) {
        const int refusals = m_createRefusals.value(screenId, 0) + 1;
        m_createRefusals.insert(screenId, refusals);
        if (refusals == MaxCreateRefusals) {
            qCWarning(lcWorkspaceRec) << "createDesktop for" << screenId << "went unanswered" << refusals
                                      << "times, no longer re-driving it for that screen";
        }
    }
    for (const QString& key : std::as_const(refusedNamedKeys)) {
        const int refusals = m_namedCreateRefusals.value(key, 0) + 1;
        m_namedCreateRefusals.insert(key, refusals);
        if (refusals == MaxCreateRefusals) {
            qCWarning(lcWorkspaceRec) << "creating named workspace" << key << "went unanswered" << refusals
                                      << "times, no longer re-driving it";
        }
    }
    // A per-screen switch KWin never echoed leaves the screen wherever it
    // was, possibly on a foreign desktop, and the resync cannot tell: it
    // re-pulls the global list, never the per-output state. The sweep below
    // re-evaluates it, spending one of the screen's correction budget.
    for (const QString& screenId : std::as_const(refusedSetCurrentScreens)) {
        const int refusals = m_setCurrentRefusals.value(screenId, 0) + 1;
        m_setCurrentRefusals.insert(screenId, refusals);
        if (refusals == MaxSetCurrentRefusals) {
            qCWarning(lcWorkspaceRec) << "switching" << screenId << "went unanswered" << refusals
                                      << "times, no longer correcting that screen until it reports again";
        }
    }

    // Maintenance runs whether or not the probe concluded. With a learned cap
    // requestCreateAt refuses at the gate, so there is no re-ask spin, while a
    // screen left with no workspace still gets the donor transfer only this
    // pass can give it.
    if (createExpired) {
        maintainInvariants();
    }
    if (!(m_map == before)) {
        bumpGeneration();
    }
    if (namedCreateExpired && m_adopted) {
        const QStringList names =
            m_declarationKwinNames.size() == m_lastIds.size() ? m_declarationKwinNames : QStringList();
        const QList<NamedWorkspace> declarations = m_declarations;
        applyNamedWorkspaces(declarations, names);
    }
    evaluateForeignAll();

    Q_EMIT resyncRequested();
    announceIfQuiet(wasPending);
}

void WorkspaceReconciler::noteCreateSucceeded()
{
    // KWin answered a create, which is proof the ceiling is not where a run of
    // expiries suggested. Drop the accumulated probe evidence and hand back the
    // configured headroom: a learned cap suspends our own creates, so without
    // this the only way out is an EXTERNAL create pushing the count past the
    // learned value, which a daemon whose creates are suspended can never
    // bring about itself.
    m_capProbeIds.clear();
    m_capProbeExpiries = 0;
    if (m_desktopCap < m_configuredCap) {
        qCWarning(lcWorkspaceRec) << "a createDesktop landed at" << m_lastIds.size()
                                  << "desktops, dropping the learned cap of" << m_desktopCap;
        m_desktopCap = m_configuredCap;
        m_capHintShown = false;
    }
}

void WorkspaceReconciler::noteCreateLanded()
{
    // KWin's ceiling is one global count with no per-screen refusal, so any
    // create it answers proves every screen's spent budget was a stall.
    m_createRefusals.clear();
    m_namedCreateRefusals.clear();
}

bool WorkspaceReconciler::hasPendingStructuralOps() const
{
    for (const auto& op : m_ledger) {
        if (op.kind != PendingOp::Kind::SetCurrent) {
            return true;
        }
    }
    return false;
}

bool WorkspaceReconciler::hasPendingRemove(const QString& desktopId) const
{
    for (const auto& op : m_ledger) {
        if (op.kind == PendingOp::Kind::Remove && !op.echoed && op.desktopId == desktopId) {
            return true;
        }
    }
    return false;
}

bool WorkspaceReconciler::hasPendingCreate(const QString& screenId) const
{
    for (const auto& op : m_ledger) {
        if (op.kind == PendingOp::Kind::Create && !op.echoed && op.screenId == screenId) {
            return true;
        }
    }
    return false;
}

int WorkspaceReconciler::pendingCreateCount() const
{
    int count = 0;
    for (const auto& op : m_ledger) {
        if (op.kind == PendingOp::Kind::Create && !op.echoed) {
            ++count;
        }
    }
    return count;
}

int WorkspaceReconciler::knownDesktopCount() const
{
    // An echoed create is already in the map but not yet in the settled list,
    // and it is no longer an open request either, so counting only those two
    // let it slip past the cap gate.
    const QSet<QString> settled(m_lastIds.cbegin(), m_lastIds.cend());
    int count = m_lastIds.size();
    const QStringList owned = m_map.allDesktopIds();
    for (const QString& id : owned) {
        if (!settled.contains(id)) {
            ++count;
        }
    }
    return count;
}

void WorkspaceReconciler::retireLedgerFor(const QString& desktopId)
{
    // An un-echoed Create carries no desktopId, so it can never match a
    // non-empty id, and this is only ever called with one.
    for (int i = m_ledger.size() - 1; i >= 0; --i) {
        if (m_ledger.at(i).desktopId == desktopId) {
            m_ledger.removeAt(i);
        }
    }
    stopLedgerTimerIfIdle();
}

int WorkspaceReconciler::matchCreateEcho(int position, const QString& name) const
{
    // Without a name or position this is the oldest open create: KWin performs
    // and echoes creations in the order it received them.
    //
    // With them, an external creation racing ours is told apart. KWin keeps
    // the name a create carries verbatim, and every create of ours carries one
    // (the workspace name, or the positional label for a dynamic desktop), so
    // an echo whose name is not the one we sent is not ours whatever its
    // position. Among the ones that match, the requested position picks.
    const bool nameKnown = !name.isNull();
    int fallback = -1;
    for (int i = 0; i < m_ledger.size(); ++i) {
        const PendingOp& op = m_ledger.at(i);
        if (op.kind != PendingOp::Kind::Create || op.echoed) {
            continue;
        }
        if (nameKnown && op.kwinName != name) {
            continue;
        }
        if (position < 0 || op.globalPosition == position) {
            return i;
        }
        if (fallback < 0) {
            fallback = i;
        }
    }
    return fallback;
}

QHash<QString, WorkspaceReconciler::PendingOp> WorkspaceReconciler::takeSettledCreates(const QStringList& newIds)
{
    // Echo-loss recovery. KWin emits desktopCreated before the list reply that
    // settles it, so in normal ordering every create has already been matched
    // (and marked echoed) by the time its settle arrives; this pairs only the
    // ones whose echo never came.
    //
    // By rank of requested position, not ledger order and not per-id distance.
    // The ledger is oldest-request-first and the settled list is KWin-position
    // ordered; those rankings agree only when the screens happened to ask in
    // slice order. Distance cannot decide it either, because a requested
    // position is only an estimate of where KWin will put the desktop. The
    // ORDER of the estimates survives, so ranks are paired.
    QHash<QString, PendingOp> matched;
    if (newIds.isEmpty()) {
        return matched;
    }
    QList<int> creates;
    for (int i = 0; i < m_ledger.size(); ++i) {
        if (m_ledger.at(i).kind == PendingOp::Kind::Create && !m_ledger.at(i).echoed) {
            creates.append(i);
        }
    }
    // Stable, so equal requested positions keep ledger (request) order.
    std::stable_sort(creates.begin(), creates.end(), [this](int a, int b) {
        return m_ledger.at(a).globalPosition < m_ledger.at(b).globalPosition;
    });
    const int pairs = qMin(creates.size(), newIds.size());
    for (int rank = 0; rank < pairs; ++rank) {
        matched.insert(newIds.at(rank), m_ledger.at(creates.at(rank)));
    }
    if (pairs > 0) {
        noteCreateSucceeded();
        noteCreateLanded();
    }
    QList<int> consumed = creates.mid(0, pairs);
    std::sort(consumed.begin(), consumed.end());
    for (int i = consumed.size() - 1; i >= 0; --i) {
        m_ledger.removeAt(consumed.at(i));
    }
    stopLedgerTimerIfIdle();
    return matched;
}

bool WorkspaceReconciler::realizeCreate(const PendingOp& op, const QString& desktopId)
{
    WorkspaceEntry entry;
    entry.desktopId = desktopId;
    entry.name = op.name;
    entry.namedId = op.namedId;
    if (m_map.knowsScreen(op.screenId)) {
        // The slot is resolved NOW from the create's intent. The slice may
        // have gained entries since the request, and a slot frozen then
        // would put the new desktop mid-slice, where an unnamed one is reaped.
        const int slot =
            op.name.isEmpty() ? m_map.sliceSize(op.screenId) : namedSliceIndex(op.screenId, op.declaredPosition);
        m_map.insert(op.screenId, slot, entry);
        return true;
    }
    // The planned owner was unplugged between the request and the landing.
    // Inserting onto a screen the map no longer knows would resurrect its
    // slice, so the desktop goes to the adoption target. A named one keeps
    // its name and is stamped home, so it rides back with the rest of that
    // output's workspaces.
    const QString target = adoptionTarget();
    qCWarning(lcWorkspaceRec) << "create" << desktopId << "landed for" << op.screenId << "which is gone, handing it to"
                              << target;
    if (target.isEmpty()) {
        return false;
    }
    if (!op.name.isEmpty()) {
        entry.homeScreenId = op.screenId;
    }
    m_map.insert(target, insertIndexBeforeTrailingEmpty(target), entry);
    return true;
}

// ── KWin echoes ─────────────────────────────────────────────────────────────

void WorkspaceReconciler::onKwinDesktopCreated(const QString& desktopId)
{
    onKwinDesktopCreatedAt(desktopId, -1, QString());
}

void WorkspaceReconciler::onKwinDesktopCreatedAt(const QString& desktopId, int position, const QString& name)
{
    // Before adoption there is no policy and no ledger: the id is in the list
    // adoptAll will distribute.
    if (desktopId.isEmpty() || !m_adopted) {
        return;
    }
    // Already ours: a late or repeated echo of a desktop the settle realized
    // first. Matching it again would consume an unrelated open create.
    if (!m_map.ownerOf(desktopId).isEmpty()) {
        return;
    }
    for (const PendingOp& op : std::as_const(m_ledger)) {
        if (op.kind == PendingOp::Kind::Create && op.echoed && op.desktopId == desktopId) {
            return;
        }
    }
    const int index = matchCreateEcho(position, name);
    if (index >= 0) {
        // Our create landed. The op stays open, marked echoed, until the
        // settled list confirms it: KWin has renumbered, the list the verbs
        // resolve against has not, and closing the structural window here
        // would release deferred verbs into the stale numbering.
        PendingOp& op = m_ledger[index];
        op.echoed = true;
        op.desktopId = desktopId;
        op.deadline = nowMs() + m_ledgerTimeoutMs;
        const PendingOp landed = op;
        noteCreateSucceeded();
        noteCreateLanded();
        if (realizeCreate(landed, desktopId)) {
            bumpGeneration();
        }
        return;
    }
    // External creation: adopt (fork 3). The settled list reply will confirm
    // ordering; adopting here keeps repairAgainst from re-reporting it.
    if (adoptExternal(desktopId)) {
        m_recentExternal.insert(desktopId, nowMs() + m_ledgerTimeoutMs);
        bumpGeneration();
    }
}

void WorkspaceReconciler::onKwinDesktopRemoved(const QString& desktopId)
{
    if (desktopId.isEmpty() || !m_adopted) {
        return;
    }
    const bool wasPending = hasPendingStructuralOps();
    // Our own Remove stays open, marked echoed, until the settle confirms it
    // (see onKwinDesktopCreatedAt). Every other entry naming the desktop dies
    // with it: a SetCurrent onto a desktop that no longer exists can never be
    // echoed, and leaving it open would block that screen's next correction
    // for the full ledger timeout and then fire a spurious resync.
    for (int i = m_ledger.size() - 1; i >= 0; --i) {
        PendingOp& op = m_ledger[i];
        if (op.desktopId != desktopId) {
            continue;
        }
        if (op.kind == PendingOp::Kind::Remove) {
            op.echoed = true;
            op.deadline = nowMs() + m_ledgerTimeoutMs;
            continue;
        }
        m_ledger.removeAt(i);
    }
    stopLedgerTimerIfIdle();
    m_racedDesktops.remove(desktopId);
    m_removalRefusals.remove(desktopId);
    m_namePushes.remove(desktopId);
    m_reportedKWinRenames.remove(desktopId);
    m_recentExternal.remove(desktopId);
    m_pinDeferredLogged.remove(desktopId);
    if (QTimer* timer = m_destroyTimers.take(desktopId)) {
        timer->stop();
        timer->deleteLater();
    }

    // KWin is the source of truth: follow, then repair invariants when the
    // list settles (slice-never-empty, trailing-empty).
    //
    // The population is dropped whatever remove() answers: the desktop is gone
    // from KWin either way, and remove() also answers false for the silent
    // stale-owner-row repair. Only the generation bump follows the return
    // value, because that is the question it answers: did the PUBLISHED map
    // change.
    const bool sliceChanged = m_map.remove(desktopId);
    m_population.remove(desktopId);
    if (sliceChanged) {
        bumpGeneration();
    }
    announceIfQuiet(wasPending);
}

int WorkspaceReconciler::matchAnsweredOp(PendingOp::Kind kind, const QString& desktopId, int position,
                                         const QString& name) const
{
    for (int i = 0; i < m_ledger.size(); ++i) {
        const PendingOp& op = m_ledger.at(i);
        if (op.kind != kind || op.replied) {
            continue;
        }
        if (kind == PendingOp::Kind::Create ? (op.globalPosition == position && op.kwinName == name)
                                            : op.desktopId == desktopId) {
            return i;
        }
    }
    return -1;
}

void WorkspaceReconciler::onCreateDesktopAnswered(uint position, const QString& name, bool answered)
{
    if (!m_adopted) {
        return;
    }
    const int index = matchAnsweredOp(PendingOp::Kind::Create, QString(), static_cast<int>(position), name);
    // No open create for it: it landed and the settle already closed it.
    if (index < 0) {
        return;
    }
    PendingOp& op = m_ledger[index];
    op.replied = true;
    if (op.echoed) {
        return; // landed; the settle closes it
    }
    if (!answered) {
        // A stall. The echo may still land before the deadline, and if it
        // does not, the expiry must not read this as KWin's ceiling.
        op.stalled = true;
        return;
    }
    const bool wasPending = hasPendingStructuralOps();
    const WorkspaceMap before = m_map;
    const PendingOp refused = m_ledger.takeAt(index);
    stopLedgerTimerIfIdle();
    // KWin returned without creating anything, which it does only at its
    // maximum: that IS the ceiling, no probe needed.
    m_desktopCap = qMax(1, knownDesktopCount());
    qCWarning(lcWorkspaceRec) << "KWin refused createDesktop at" << m_desktopCap
                              << "desktops, learned as the compositor's cap";
    if (refused.name.isEmpty()) {
        m_createRefusals.insert(refused.screenId, m_createRefusals.value(refused.screenId, 0) + 1);
    } else {
        const QString key = refused.namedId.isEmpty() ? refused.name : refused.namedId;
        m_namedCreateRefusals.insert(key, m_namedCreateRefusals.value(key, 0) + 1);
    }
    if (!m_capHintShown) {
        m_capHintShown = true;
        Q_EMIT capReached();
    }
    // At the ceiling a screen left with no workspace is given one by a donor.
    maintainInvariants();
    if (!(m_map == before)) {
        bumpGeneration();
    }
    evaluateForeignAll();
    announceIfQuiet(wasPending);
}

void WorkspaceReconciler::onRemoveDesktopAnswered(const QString& desktopId, bool answered)
{
    if (!m_adopted) {
        return;
    }
    const int index = matchAnsweredOp(PendingOp::Kind::Remove, desktopId, -1, QString());
    if (index < 0) {
        return;
    }
    PendingOp& op = m_ledger[index];
    op.replied = true;
    if (op.echoed) {
        return;
    }
    if (!answered) {
        op.stalled = true;
        return;
    }
    // KWin returned and kept the desktop: its last one, or an id it does not
    // know. The same refusal an expiry would have counted, a timeout sooner.
    const bool wasPending = hasPendingStructuralOps();
    m_ledger.removeAt(index);
    stopLedgerTimerIfIdle();
    m_racedDesktops.remove(desktopId);
    Q_EMIT removalAbandoned(desktopId);
    const int refusals = m_removalRefusals.value(desktopId, 0) + 1;
    m_removalRefusals.insert(desktopId, refusals);
    qCWarning(lcWorkspaceRec) << "KWin refused removing" << desktopId << "(" << refusals << "of" << MaxRemovalRefusals
                              << ")";
    if (refusals < MaxRemovalRefusals && isReapable(desktopId)) {
        scheduleDestroyCheck(desktopId);
    }
    evaluateForeignAll();
    announceIfQuiet(wasPending);
}

void WorkspaceReconciler::onKwinDesktopRenamed(const QString& desktopId, const QString& rawName)
{
    if (desktopId.isEmpty() || !m_adopted) {
        return;
    }
    const int position = static_cast<int>(m_lastIds.indexOf(desktopId));
    if (position >= 0 && m_lastRawNames.size() == m_lastIds.size()) {
        m_lastRawNames[position] = rawName;
    }
    const auto push = m_namePushes.constFind(desktopId);
    if (push != m_namePushes.constEnd() && push->name == rawName) {
        m_namePushes.remove(desktopId); // our own push landing
        return;
    }
    const WorkspaceEntry entry = m_map.entryFor(desktopId);
    if (entry.desktopId.isEmpty()) {
        return;
    }
    if (entry.name.isEmpty()) {
        refreshPositionalLabels();
        return;
    }
    // A named workspace's desktop got a name a user chose, with no push of
    // ours in flight for it: renamed on KWin's side.
    if (rawName == entry.name) {
        m_reportedKWinRenames.remove(desktopId);
        return;
    }
    if (rawName.isEmpty() || isPositionalLabel(rawName) || m_namePushes.contains(desktopId)
        || m_reportedKWinRenames.value(desktopId) == rawName) {
        return;
    }
    m_reportedKWinRenames.insert(desktopId, rawName);
    Q_EMIT namedWorkspaceRenamedInKWin(entry.namedId, desktopId, rawName);
}

QString WorkspaceReconciler::recordScreenDesktop(const QString& screenId, int desktop, bool restoresCorrectionBudget)
{
    // D-Bus boundary: the report comes from the effect over the wire. An empty
    // screen id or a non-positive desktop is not a state worth recording — it
    // would poison currentDesktopIdOf and the verbs resolved through it.
    if (screenId.isEmpty() || desktop < 1) {
        qCWarning(lcWorkspaceRec) << "ignoring malformed screen desktop report, screen =" << screenId
                                  << "desktop =" << desktop;
        return QString();
    }
    // A report is the screen telling us where it is, so the correction budget
    // an unechoed switch spent comes back. The one exception is the report
    // that answers a correction by refusing it: that answer spends the budget
    // instead (onScreenDesktopReport), or a correction the compositor always
    // refuses would be re-issued on every answer forever.
    if (restoresCorrectionBudget) {
        m_setCurrentRefusals.remove(screenId);
    }

    if (desktop > m_lastIds.size()) {
        // A desktop the settled list does not have yet: the list is about to
        // grow. Resolved by the next settle rather than guessed now.
        m_pendingReportByScreen.insert(screenId, desktop);
        m_currentIdByScreen.remove(screenId);
        return QString();
    }
    m_pendingReportByScreen.remove(screenId);
    const QString desktopId = m_lastIds.at(desktop - 1);
    setCurrentFor(screenId, desktopId);

    // The snap-back memo is recorded for EVERY report that lands the screen on
    // one of its own desktops, echo or not: a matched SetCurrent echo is
    // exactly the case where the screen just arrived somewhere it owns.
    if (m_map.ownerOf(desktopId) == screenId) {
        m_lastOwnedByScreen.insert(screenId, desktopId);
    }
    return desktopId;
}

int WorkspaceReconciler::openSetCurrentIndex(const QString& screenId) const
{
    for (int i = 0; i < m_ledger.size(); ++i) {
        const PendingOp& op = m_ledger.at(i);
        if (op.kind == PendingOp::Kind::SetCurrent && op.screenId == screenId) {
            return i;
        }
    }
    return -1;
}

bool WorkspaceReconciler::onScreenDesktopReport(const QString& screenId, int desktop)
{
    // The effect answers every switch request exactly once: with the switch's
    // own report when it switched, otherwise with a report of the desktop the
    // output still shows (refused under KWin's global mode, an unknown id, a
    // correction deferred under Show Desktop, a script that switched back).
    // So a report for a screen with a switch open that names a DIFFERENT
    // desktop is that switch's answer, and the switch did not happen. Whether
    // it refuses a correction has to be known before recording, because
    // recording otherwise hands the correction budget back.
    const int openBefore = openSetCurrentIndex(screenId);
    const QString reportedId = (desktop >= 1 && desktop <= m_lastIds.size()) ? m_lastIds.at(desktop - 1) : QString();
    const bool refusesCorrection = openBefore >= 0 && !reportedId.isEmpty()
        && m_ledger.at(openBefore).desktopId != reportedId && !m_ledger.at(openBefore).replaceable
        && m_ledger.at(openBefore).supersededAnswers == 0;

    const QString desktopId = recordScreenDesktop(screenId, desktop, !refusesCorrection);
    if (desktopId.isEmpty()) {
        return false;
    }
    const int open = openSetCurrentIndex(screenId);
    if (open >= 0 && m_ledger.at(open).desktopId != desktopId && m_ledger.at(open).supersededAnswers > 0) {
        // The answer to a press this switch replaced (key repeat), not to
        // this one: the entry keeps waiting for its own.
        --m_ledger[open].supersededAnswers;
    } else if (open >= 0) {
        const PendingOp op = m_ledger.at(open);
        m_ledger.removeAt(open);
        stopLedgerTimerIfIdle();
        if (op.desktopId == desktopId) {
            return true; // echo of our own switch — no reactive policy
        }
        // Answered, not switched. Retiring the entry here instead of waiting
        // for its expiry keeps the screen from being blocked for the whole
        // ledger timeout. A refused correction spends the screen's budget the
        // way an expired one does, so a correction the compositor always
        // refuses stops after MaxSetCurrentRefusals answers; a refused user
        // switch spends nothing (the user's next press is the retry).
        if (refusesCorrection) {
            const int refusals = m_setCurrentRefusals.value(screenId, 0) + 1;
            m_setCurrentRefusals.insert(screenId, refusals);
            if (refusals == MaxSetCurrentRefusals) {
                qCWarning(lcWorkspaceRec) << "switching" << screenId << "was refused" << refusals
                                          << "times, no longer correcting that screen until it reports again";
            }
        }
        qCDebug(lcWorkspaceRec) << "switch of" << screenId << "to" << op.desktopId << "answered with" << desktopId
                                << "instead";
    }

    if (!m_adopted) {
        return false;
    }
    const QString owner = m_map.ownerOf(desktopId);
    // A desktop someone created elsewhere and is showing on THIS screen was
    // adopted onto the focused one; it belongs here, not snapped away.
    if (owner != screenId && transferExternalToShowingScreen(desktopId)) {
        bumpGeneration();
        return false;
    }
    if (!owner.isEmpty() && owner != screenId) {
        evaluateForeign(screenId);
    }
    return false;
}

void WorkspaceReconciler::onScreenDesktopSeeded(const QString& screenId, int desktop)
{
    const QString desktopId = recordScreenDesktop(screenId, desktop);
    if (desktopId.isEmpty() || !m_adopted) {
        return;
    }
    const QString owner = m_map.ownerOf(desktopId);
    if (!owner.isEmpty() && owner != screenId) {
        evaluateForeign(screenId);
    }
}

void WorkspaceReconciler::setCurrentFor(const QString& screenId, const QString& desktopId)
{
    const QString previous = m_currentIdByScreen.value(screenId);
    m_currentIdByScreen.insert(screenId, desktopId);
    // A shown desktop is never reaped, so the one this screen just left may
    // have become surplus the moment it stopped being shown.
    if (m_adopted && !previous.isEmpty() && previous != desktopId && isReapable(previous)) {
        scheduleDestroyCheck(previous);
    }
}

void WorkspaceReconciler::evaluateForeign(const QString& screenId)
{
    // A structural op in flight makes the numbering transient: KWin renumbers
    // on both create and remove, and a current resolved mid-window can name
    // the desktop one slot off. The settle that closes the window re-sweeps
    // every screen, and so does every expiry.
    if (!m_adopted || hasPendingStructuralOps()) {
        return;
    }
    if (m_setCurrentRefusals.value(screenId, 0) >= MaxSetCurrentRefusals) {
        return;
    }
    const QString desktopId = currentDesktopIdOf(screenId);
    if (desktopId.isEmpty()) {
        return;
    }
    const QString owner = m_map.ownerOf(desktopId);
    if (owner.isEmpty() || owner == screenId) {
        return;
    }
    // One correction per external event: while a SetCurrent for this screen
    // is open, further foreign reports are ignored; the open op's echo
    // retires it and the next report re-evaluates.
    for (const auto& op : m_ledger) {
        if (op.kind == PendingOp::Kind::SetCurrent && op.screenId == screenId) {
            return;
        }
    }
    Q_EMIT foreignSwitchDetected(screenId, desktopId, owner);
}

void WorkspaceReconciler::evaluateForeignAll()
{
    const QStringList screens = m_map.screenOrder();
    for (const QString& screenId : screens) {
        evaluateForeign(screenId);
    }
}

// ── Verb support ────────────────────────────────────────────────────────────

QString WorkspaceReconciler::currentDesktopIdOf(const QString& screenId) const
{
    return m_currentIdByScreen.value(screenId);
}

QHash<QString, int> WorkspaceReconciler::currentIndexByScreen() const
{
    QHash<QString, int> result;
    for (auto it = m_currentIdByScreen.constBegin(); it != m_currentIdByScreen.constEnd(); ++it) {
        const int index = m_lastIds.indexOf(it.value());
        if (index >= 0) {
            result.insert(it.key(), index + 1);
        }
    }
    return result;
}

QString WorkspaceReconciler::desktopIdAtOffset(const QString& screenId, int delta) const
{
    return desktopIdAtOffsetFrom(screenId, currentDesktopIdOf(screenId), delta);
}

QString WorkspaceReconciler::desktopIdAtOffsetFrom(const QString& screenId, const QString& fromDesktopId,
                                                   int delta) const
{
    if (fromDesktopId.isEmpty()) {
        return QString();
    }
    // Resolve within the screen's OWN slice; a screen currently showing a
    // foreign desktop (pre-snap-back window) resolves nothing.
    if (m_map.ownerOf(fromDesktopId) != screenId) {
        return QString();
    }
    return desktopIdAtSliceIndex(screenId, m_map.sliceIndexOf(fromDesktopId) + delta);
}

QString WorkspaceReconciler::desktopIdAtSliceIndex(const QString& screenId, int sliceIndex) const
{
    const auto entries = m_map.slice(screenId);
    if (sliceIndex < 0 || sliceIndex >= entries.size()) {
        return QString(); // slice edge: no wrap (niri semantics)
    }
    return entries.at(sliceIndex).desktopId;
}

QString WorkspaceReconciler::inFlightSetCurrentTarget(const QString& screenId) const
{
    for (const auto& op : m_ledger) {
        if (op.kind == PendingOp::Kind::SetCurrent && op.screenId == screenId) {
            return op.desktopId;
        }
    }
    return QString();
}

bool WorkspaceReconciler::issueSetCurrent(const QString& screenId, const QString& desktopId)
{
    if (screenId.isEmpty() || desktopId.isEmpty()) {
        return false;
    }
    // Refuse an UNTRANSLATABLE target before it reaches the ledger. The map
    // can hold an id KWin has echoed but not yet settled, and the desktop int
    // the controller must resolve to drive the effect comes from the settled
    // list. Ledgering such a switch would short-circuit this screen's foreign
    // evaluation for a full ledger timeout and then expire into a spurious
    // resync. The next settle makes the id live and the caller asks again.
    if (!m_lastIds.contains(desktopId)) {
        qCWarning(lcWorkspaceRec) << "not switching" << screenId << "to" << desktopId
                                  << "- that desktop is not in the settled list yet";
        return false;
    }
    for (const auto& op : m_ledger) {
        if (op.kind == PendingOp::Kind::SetCurrent && op.screenId == screenId) {
            return false; // one correction per screen in flight
        }
    }
    PendingOp op;
    op.kind = PendingOp::Kind::SetCurrent;
    op.screenId = screenId;
    op.desktopId = desktopId;
    ledgerAdd(op);
    Q_EMIT requestSetCurrent(screenId, desktopId);
    return true;
}

bool WorkspaceReconciler::issueSetCurrentReplacing(const QString& screenId, const QString& desktopId)
{
    if (screenId.isEmpty() || desktopId.isEmpty() || !m_lastIds.contains(desktopId)) {
        return false;
    }
    int replaced = -1;
    for (int i = 0; i < m_ledger.size(); ++i) {
        const PendingOp& op = m_ledger.at(i);
        if (op.kind == PendingOp::Kind::SetCurrent && op.screenId == screenId) {
            // An owner-wins correction is the loop breaker and is never
            // pre-empted by a navigation press.
            if (!op.replaceable) {
                return false;
            }
            replaced = i;
            break;
        }
    }
    // The effect still answers the request being replaced, so the new entry
    // owes that answer too and must not read it as its own refusal.
    int supersededAnswers = 0;
    if (replaced >= 0) {
        supersededAnswers = m_ledger.at(replaced).supersededAnswers + 1;
        m_ledger.removeAt(replaced);
    }
    PendingOp op;
    op.kind = PendingOp::Kind::SetCurrent;
    op.screenId = screenId;
    op.desktopId = desktopId;
    op.replaceable = true;
    op.supersededAnswers = supersededAnswers;
    ledgerAdd(op);
    Q_EMIT requestSetCurrent(screenId, desktopId);
    return true;
}

bool WorkspaceReconciler::snapBack(const QString& screenId)
{
    const QString currentId = currentDesktopIdOf(screenId);
    if (!currentId.isEmpty() && m_map.ownerOf(currentId) == screenId) {
        return false; // already home
    }
    QString target = m_lastOwnedByScreen.value(screenId);
    if (target.isEmpty() || m_map.ownerOf(target) != screenId) {
        target = desktopIdAtSliceIndex(screenId, 0);
    }
    if (target.isEmpty()) {
        return false;
    }
    return issueSetCurrent(screenId, target);
}

bool WorkspaceReconciler::reorderCurrentWorkspace(const QString& screenId, int delta)
{
    const QString currentId = currentDesktopIdOf(screenId);
    if (currentId.isEmpty() || m_map.ownerOf(currentId) != screenId) {
        return false;
    }
    // The trailing empty is the invariant, not a workspace the user arranges:
    // moving it up leaves it mid-slice, where it is reaped and recreated, and
    // moving anything past it does the same to it.
    const QString trailing = trailingEmptyOf(screenId);
    if (currentId == trailing) {
        return false;
    }
    const int size = m_map.sliceSize(screenId);
    const int lastSlot = trailing.isEmpty() ? size - 1 : size - 2;
    const int target = m_map.sliceIndexOf(currentId) + delta;
    if (target < 0 || target > lastSlot) {
        return false;
    }
    if (!m_map.reorderWithinSlice(currentId, target)) {
        return false;
    }
    maintainScreen(screenId);
    bumpGeneration();
    const WorkspaceEntry entry = m_map.entryFor(currentId);
    if (!entry.name.isEmpty()) {
        Q_EMIT namedWorkspaceMoved(entry.namedId, entry.name, screenId, m_map.sliceIndexOf(currentId));
    }
    return true;
}

QString WorkspaceReconciler::transferCurrentWorkspace(const QString& screenId, const QString& targetScreenId)
{
    if (targetScreenId.isEmpty() || targetScreenId == screenId || !m_map.knowsScreen(targetScreenId)) {
        return QString();
    }
    const QString currentId = currentDesktopIdOf(screenId);
    if (currentId.isEmpty() || m_map.ownerOf(currentId) != screenId) {
        return QString();
    }
    if (m_map.sliceSize(screenId) <= 1) {
        return QString(); // a screen never gives up its last desktop
    }
    // The trailing empty is the source's invariant: handing it over leaves the
    // source without one (a create to replace it) and the target with a
    // surplus empty (a removal), all for moving nothing.
    if (currentId == trailingEmptyOf(screenId)) {
        return QString();
    }
    const int sourceIndex = m_map.sliceIndexOf(currentId);
    if (!m_map.transfer(currentId, targetScreenId, insertIndexBeforeTrailingEmpty(targetScreenId))) {
        return QString();
    }
    // A deliberate move overrides hotplug memory: the home stamp exists so an
    // output's own workspaces come back when it is replugged, and keeping it
    // through a user transfer would yank the workspace off the screen the user
    // just put it on the next time that output reappears.
    m_map.setHomeScreen(currentId, QString());
    // The source lands on the workspace that took the moved one's slot (its
    // old neighbour below, or above when it was the last), not on whatever its
    // slice happens to start with.
    const QList<WorkspaceEntry> remaining = m_map.slice(screenId);
    if (!remaining.isEmpty()) {
        issueSetCurrent(screenId, remaining.at(qMin(sourceIndex, static_cast<int>(remaining.size()) - 1)).desktopId);
    }
    maintainScreen(screenId);
    maintainScreen(targetScreenId);
    bumpGeneration();
    const WorkspaceEntry entry = m_map.entryFor(currentId);
    if (!entry.name.isEmpty()) {
        Q_EMIT namedWorkspaceMoved(entry.namedId, entry.name, targetScreenId, m_map.sliceIndexOf(currentId));
    }
    return currentId;
}

// ── Settled list reply ──────────────────────────────────────────────────────

void WorkspaceReconciler::onDesktopListSettled(const QStringList& ids, const QStringList& rawNames)
{
    // Before adoption, adoptAll is handed the list itself.
    if (!m_adopted) {
        return;
    }
    if (ids == m_lastIds) {
        // A names-only change (a rename on either side) leaves the ids alone
        // but can still leave a positional label stale.
        if (rawNames.size() == ids.size() && rawNames != m_lastRawNames) {
            m_lastRawNames = rawNames;
            refreshPositionalLabels();
        }
        return;
    }

    // KWin never has zero desktops, so an empty settled list while we know of
    // some is a failed read upstream, not a fact. Acting on it would drop
    // every slice and have the engines reap all per-desktop state.
    if (ids.isEmpty() && !m_lastIds.isEmpty()) {
        qCWarning(lcWorkspaceRec) << "ignoring an empty desktop list while" << m_lastIds.size()
                                  << "desktops are known, treating it as a failed read";
        Q_EMIT resyncRequested();
        return;
    }

    const bool wasPending = hasPendingStructuralOps();

    // Position index built once: every membership question below is asked
    // per entry, and a list lookup each time is quadratic in the count.
    QHash<QString, int> indexOfId;
    indexOfId.reserve(ids.size());
    for (int i = 0; i < ids.size(); ++i) {
        indexOfId.insert(ids.at(i), i);
    }
    const QStringList oldIds = m_lastIds;
    const QSet<QString> previousIds(oldIds.constBegin(), oldIds.constEnd());
    m_lastIds = ids;
    m_lastRawNames = rawNames.size() == ids.size() ? rawNames : QStringList();

    // Headroom came back: a shorter desktop list is the one event that can
    // turn a standing create refusal (KWin at its ceiling) back into a create
    // it will answer, and nothing else would ever hand the budget back.
    if (ids.size() < oldIds.size()) {
        m_createRefusals.clear();
        m_namedCreateRefusals.clear();
    }

    // Cap self-heal. A learned ceiling the live count now exceeds was a
    // mislearn (a transient stall, or the compositor's cap changed), so the
    // configured value comes back. And the live count is itself proof: a
    // ceiling below it cannot be right whatever it was set to.
    if (m_desktopCap < m_configuredCap && ids.size() > m_desktopCap) {
        qCWarning(lcWorkspaceRec) << "desktop count" << ids.size() << "exceeds the learned cap" << m_desktopCap
                                  << ", reverting to the configured ceiling";
        m_desktopCap = m_configuredCap;
        m_capHintShown = false;
    }
    if (ids.size() > m_desktopCap) {
        m_desktopCap = static_cast<int>(ids.size());
    }

    // Each screen's current desktop is held by id, so survivors need no
    // renumber at all. A current desktop that vanished moves where KWin moves
    // that output: the next surviving desktop, or the last one before it.
    // Erasing it instead left the screen with no current until a report that,
    // for a desktop number that did not change, never comes.
    const PhosphorEngine::DesktopRemap remap = PhosphorEngine::DesktopRemap::fromIds(oldIds, ids);
    for (auto it = m_currentIdByScreen.begin(); it != m_currentIdByScreen.end();) {
        if (indexOfId.contains(it.value())) {
            ++it;
            continue;
        }
        const int oldPosition = static_cast<int>(oldIds.indexOf(it.value())) + 1;
        const int successor = oldPosition > 0 ? remap.successor(oldPosition) : 0;
        if (successor > 0 && successor <= ids.size()) {
            it.value() = ids.at(successor - 1);
            ++it;
        } else {
            it = m_currentIdByScreen.erase(it);
        }
    }

    // Per-desktop bookkeeping for ids KWin no longer lists has nothing left to
    // track. A desktop removed through kwinDesktopRemoved is already cleared
    // there; this catches the ones that only ever show up as a list delta.
    for (auto it = m_removalRefusals.begin(); it != m_removalRefusals.end();) {
        it = indexOfId.contains(it.key()) ? std::next(it) : m_removalRefusals.erase(it);
    }
    for (auto it = m_namePushes.begin(); it != m_namePushes.end();) {
        it = indexOfId.contains(it.key()) ? std::next(it) : m_namePushes.erase(it);
    }
    for (auto it = m_reportedKWinRenames.begin(); it != m_reportedKWinRenames.end();) {
        it = indexOfId.contains(it.key()) ? std::next(it) : m_reportedKWinRenames.erase(it);
    }
    for (auto it = m_population.begin(); it != m_population.end();) {
        it = indexOfId.contains(it.key()) ? std::next(it) : m_population.erase(it);
    }
    for (auto it = m_racedDesktops.begin(); it != m_racedDesktops.end();) {
        it = indexOfId.contains(*it) ? std::next(it) : m_racedDesktops.erase(it);
    }
    for (auto it = m_pinDeferredLogged.begin(); it != m_pinDeferredLogged.end();) {
        it = indexOfId.contains(*it) ? std::next(it) : m_pinDeferredLogged.erase(it);
    }

    // The settle is what closes an echoed op, and what proves an un-echoed
    // one landed or died: a Remove whose desktop is gone succeeded even if its
    // echo was lost, and a switch onto a desktop that is gone can never be
    // echoed. Retiring them here keeps them from expiring into a refusal and a
    // resync for work that is already done.
    for (int i = m_ledger.size() - 1; i >= 0; --i) {
        const PendingOp& op = m_ledger.at(i);
        const bool listed = indexOfId.contains(op.desktopId);
        bool retire = false;
        switch (op.kind) {
        case PendingOp::Kind::Create:
            retire = op.echoed && listed;
            break;
        case PendingOp::Kind::Remove:
            retire = !listed;
            break;
        case PendingOp::Kind::SetCurrent:
            retire = !listed;
            break;
        }
        if (retire) {
            m_ledger.removeAt(i);
        }
    }
    stopLedgerTimerIfIdle();

    const QStringList unowned = m_map.repairAgainst(ids);
    // repairAgainst reports in KWin order, so this keeps the new ids in
    // settled-list index order, the ranking takeSettledCreates pairs against.
    QStringList newUnowned;
    for (const QString& id : unowned) {
        if (!previousIds.contains(id)) {
            newUnowned.append(id);
        }
    }
    const QHash<QString, PendingOp> settledCreates = takeSettledCreates(newUnowned);
    const qint64 now = nowMs();
    for (const QString& id : unowned) {
        // Creates are retired by MATCHING, never by counting: counting the
        // size delta once retired a second op per landed desktop, which left
        // the survivor's echo with no ledger entry to match.
        const auto matched = settledCreates.constFind(id);
        if (matched != settledCreates.constEnd() && realizeCreate(*matched, id)) {
            continue;
        }
        if (adoptExternal(id)) {
            m_recentExternal.insert(id, now + m_ledgerTimeoutMs);
        }
    }
    // Populations are NOT cleared for adopted ids: the controller's census is
    // the source of truth and a desktop that already carried windows when it
    // was adopted must keep its count.

    // Reports that arrived ahead of the list they number against.
    for (auto it = m_pendingReportByScreen.begin(); it != m_pendingReportByScreen.end();) {
        if (it.value() > ids.size()) {
            ++it;
            continue;
        }
        const QString desktopId = ids.at(it.value() - 1);
        setCurrentFor(it.key(), desktopId);
        if (m_map.ownerOf(desktopId) == it.key()) {
            m_lastOwnedByScreen.insert(it.key(), desktopId);
        }
        it = m_pendingReportByScreen.erase(it);
    }

    // An external desktop one other screen is showing belongs to that screen.
    // Done before maintenance and the owner-wins sweep, which would otherwise
    // reap it or snap that screen away from it.
    const QStringList recent = m_recentExternal.keys();
    for (const QString& id : recent) {
        transferExternalToShowingScreen(id);
    }
    for (auto it = m_recentExternal.begin(); it != m_recentExternal.end();) {
        it = it.value() <= now ? m_recentExternal.erase(it) : std::next(it);
    }

    maintainInvariants();
    refreshPositionalLabels();

    // Owner-wins re-check against the settled truth: reports that arrived
    // during a structural window were recorded without policy.
    evaluateForeignAll();

    // Announced on every settle that gets this far, and truthfully: the
    // published map carries each desktop's 1-based position, and a settle
    // past the early return above always moves at least one (an id was
    // added, removed or reordered), so the payload a consumer diffs changes
    // even when no slice did. Deferred verbs do NOT ride this; they wait for
    // structuralOpsSettled.
    bumpGeneration();
    announceIfQuiet(wasPending);
}

QString WorkspaceReconciler::adoptionTarget() const
{
    if (m_focusedScreenProvider) {
        const QString provided = m_focusedScreenProvider();
        if (!provided.isEmpty() && m_map.knowsScreen(provided)) {
            return provided;
        }
    }
    if (!m_focusedScreen.isEmpty() && m_map.knowsScreen(m_focusedScreen)) {
        return m_focusedScreen;
    }
    const QStringList order = m_map.screenOrder();
    return order.isEmpty() ? QString() : order.first();
}

bool WorkspaceReconciler::adoptExternal(const QString& desktopId)
{
    // An external desktop is adopted as an ordinary dynamic one, so once it is
    // empty and nobody shows it the destroy sweep reaps it like any other
    // (niri semantics; a Pager "add desktop" with nothing moved onto it does
    // not survive). One a screen is showing is kept, and one shown by a
    // single other screen moves there (transferExternalToShowingScreen).
    const QString target = adoptionTarget();
    if (target.isEmpty()) {
        qCWarning(lcWorkspaceRec) << "no screen available to adopt desktop" << desktopId;
        return false;
    }
    WorkspaceEntry entry;
    entry.desktopId = desktopId;
    m_map.insert(target, insertIndexBeforeTrailingEmpty(target), entry);
    return true;
}

bool WorkspaceReconciler::transferExternalToShowingScreen(const QString& desktopId)
{
    const auto recent = m_recentExternal.constFind(desktopId);
    if (recent == m_recentExternal.constEnd()) {
        return false;
    }
    if (recent.value() <= nowMs()) {
        m_recentExternal.remove(desktopId);
        return false;
    }
    const QString owner = m_map.ownerOf(desktopId);
    const WorkspaceEntry entry = m_map.entryFor(desktopId);
    if (owner.isEmpty() || !entry.name.isEmpty() || !isDesktopEmpty(desktopId)) {
        return false;
    }
    QStringList showing;
    for (auto it = m_currentIdByScreen.constBegin(); it != m_currentIdByScreen.constEnd(); ++it) {
        if (it.value() == desktopId) {
            showing.append(it.key());
        }
    }
    // Exactly one screen, and not the adopter: with two screens on it the
    // claim is ambiguous and the adopter keeps it.
    if (showing.size() != 1 || showing.first() == owner || !m_map.knowsScreen(showing.first())) {
        return false;
    }
    const QString target = showing.first();
    m_map.transfer(desktopId, target, insertIndexBeforeTrailingEmpty(target));
    m_recentExternal.remove(desktopId);
    m_lastOwnedByScreen.insert(target, desktopId);
    maintainScreen(owner);
    maintainScreen(target);
    return true;
}

// ── Population and lifecycle ────────────────────────────────────────────────

bool WorkspaceReconciler::isDesktopEmpty(const QString& desktopId) const
{
    return m_population.value(desktopId, 0) <= 0;
}

bool WorkspaceReconciler::isShown(const QString& desktopId) const
{
    for (auto it = m_currentIdByScreen.constBegin(); it != m_currentIdByScreen.constEnd(); ++it) {
        if (it.value() == desktopId) {
            return true;
        }
    }
    return false;
}

bool WorkspaceReconciler::isReapable(const QString& desktopId) const
{
    const QString owner = m_map.ownerOf(desktopId);
    if (owner.isEmpty()) {
        return false;
    }
    const WorkspaceEntry entry = m_map.entryFor(desktopId);
    // Named: destroy-exempt. Home-stamped: displaced by an unplug and riding
    // home intact on replug, so the foster screen does not reap it.
    if (!entry.name.isEmpty() || !entry.homeScreenId.isEmpty()) {
        return false;
    }
    if (!isDesktopEmpty(desktopId) || isShown(desktopId)) {
        return false;
    }
    const QList<WorkspaceEntry> entries = m_map.slice(owner);
    if (entries.size() <= 1) {
        return false; // a slice never becomes empty
    }
    return entries.last().desktopId != desktopId; // the trailing empty is the invariant
}

int WorkspaceReconciler::insertIndexBeforeTrailingEmpty(const QString& screenId) const
{
    const int size = m_map.sliceSize(screenId);
    return trailingEmptyOf(screenId).isEmpty() ? size : qMax(0, size - 1);
}

int WorkspaceReconciler::kwinOrderedSlot(const QString& screenId, const QString& desktopId) const
{
    const int position = static_cast<int>(m_lastIds.indexOf(desktopId));
    const QList<WorkspaceEntry> entries = m_map.slice(screenId);
    for (int i = 0; i < entries.size(); ++i) {
        const int other = static_cast<int>(m_lastIds.indexOf(entries.at(i).desktopId));
        if (other > position) {
            return i;
        }
    }
    return entries.size();
}

QString WorkspaceReconciler::trailingEmptyOf(const QString& screenId) const
{
    const auto entries = m_map.slice(screenId);
    if (entries.isEmpty()) {
        return QString();
    }
    const WorkspaceEntry& last = entries.last();
    if (last.name.isEmpty() && isDesktopEmpty(last.desktopId)) {
        return last.desktopId;
    }
    return QString();
}

void WorkspaceReconciler::onPopulationChanged(const QString& desktopId, int windowCount)
{
    const int previous = m_population.value(desktopId, 0);
    if (previous == windowCount) {
        return;
    }
    m_population.insert(desktopId, windowCount);
    // The population is the usual reason KWin declines a removal, so any change
    // to it restores this desktop's refusal budget.
    m_removalRefusals.remove(desktopId);

    // Before adoption the census is only recorded: the map it would act on is
    // an unreconciled candidate.
    if (!m_adopted) {
        return;
    }
    const QString owner = m_map.ownerOf(desktopId);
    if (owner.isEmpty()) {
        return;
    }

    // Destroy race (plan §4.3 step 4): a window mapped onto this desktop
    // while our removeDesktop for it is in flight. Too late to cancel — KWin
    // will sweep the window to an arbitrary neighbour when the removal lands.
    // An edge, not a level: the latch clears when the Remove leaves the
    // ledger (echo, expiry, or the desktop's own removal).
    if (windowCount > 0 && !m_racedDesktops.contains(desktopId) && hasPendingRemove(desktopId)) {
        m_racedDesktops.insert(desktopId);
        Q_EMIT removalRaceDetected(desktopId, owner);
    }

    if (windowCount > 0 && previous <= 0) {
        // Occupying the trailing empty appends the next one (create-on-occupy).
        const auto entries = m_map.slice(owner);
        if (!entries.isEmpty() && entries.last().desktopId == desktopId) {
            maintainScreen(owner);
        }
        return;
    }

    if (windowCount <= 0 && previous > 0) {
        scheduleDestroyCheck(desktopId);
    }
}

void WorkspaceReconciler::scheduleDestroyCheck(const QString& desktopId)
{
    if (!m_adopted) {
        return;
    }
    // Debounce: window moves between desktops arrive as leave+arrive pairs.
    QTimer* timer = m_destroyTimers.value(desktopId);
    if (!timer) {
        timer = new QTimer(this);
        timer->setSingleShot(true);
        m_destroyTimers.insert(desktopId, timer);
        connect(timer, &QTimer::timeout, this, [this, desktopId]() {
            if (QTimer* fired = m_destroyTimers.take(desktopId)) {
                fired->deleteLater();
            }
            // Our own Remove for this desktop is already in flight: issuing a
            // second one would have KWin answer one echo and leave the other
            // to expire, which fires a spurious resync.
            if (hasPendingRemove(desktopId)) {
                return;
            }
            // Refusal budget spent (see MaxRemovalRefusals). Checked HERE and
            // not only at the re-arm because maintainScreen's surplus scan
            // re-arms this same check on every settle.
            if (m_removalRefusals.value(desktopId, 0) >= MaxRemovalRefusals) {
                return;
            }
            // Last-moment re-check (plan §4.3 destroy step 2), against the
            // state NOW: the desktop may have filled, become shown, been
            // named, or been displaced by an unplug since the timer was armed.
            if (!isReapable(desktopId)) {
                return;
            }
            PendingOp op;
            op.kind = PendingOp::Kind::Remove;
            op.desktopId = desktopId;
            ledgerAdd(op);
            Q_EMIT requestRemoveDesktop(desktopId);
        });
    }
    timer->setInterval(m_destroyDebounceMs);
    timer->start();
}

void WorkspaceReconciler::requestCreateAt(const QString& screenId, int declaredPosition, const QString& name,
                                          const QString& namedId)
{
    if (!m_adopted) {
        return;
    }
    // Refusal budget spent (see MaxCreateRefusals). Checked HERE rather than
    // only at the post-expiry re-drive, because maintainScreen also runs off
    // every settle and every population edge. A named create spends its
    // declaration's budget instead of the screen's: the screen budget accrues
    // from unrelated trailing-empty repair, and must not swallow a workspace
    // the user just declared.
    if (name.isEmpty()) {
        if (m_createRefusals.value(screenId, 0) >= MaxCreateRefusals) {
            return;
        }
    } else if (m_namedCreateRefusals.value(namedId.isEmpty() ? name : namedId, 0) >= MaxCreateRefusals) {
        return;
    }
    // Count every desktop KWin has or still owes us: the settled list, the
    // echoed creates not in it yet, and the open requests. Gating on the list
    // alone let a burst request past the ceiling and then learn a bogus cap
    // from the refusals.
    if (knownDesktopCount() + pendingCreateCount() >= m_desktopCap) {
        if (!m_capHintShown) {
            m_capHintShown = true;
            Q_EMIT capReached();
        }
        return;
    }
    m_capHintShown = false;
    const int sliceIndex = name.isEmpty() ? m_map.sliceSize(screenId) : namedSliceIndex(screenId, declaredPosition);
    const uint globalPosition = kwinPositionForInsert(screenId, sliceIndex);
    PendingOp op;
    op.kind = PendingOp::Kind::Create;
    op.screenId = screenId;
    op.declaredPosition = declaredPosition;
    op.globalPosition = static_cast<int>(globalPosition);
    op.name = name;
    op.namedId = namedId;
    // A dynamic desktop is created under the label KWin would give it anyway.
    // Sending it ourselves makes the echo's name one we chose, so the echo
    // matches this create exactly even when the label provider and KWin's
    // locale disagree, and a desktop a user named elsewhere never does.
    op.kwinName = name.isEmpty() ? defaultDesktopName(static_cast<int>(globalPosition) + 1) : name;
    ledgerAdd(op);
    Q_EMIT requestCreateDesktop(globalPosition, op.kwinName);
}

uint WorkspaceReconciler::kwinPositionForInsert(const QString& screenId, int sliceIndex) const
{
    // Anchored on KWin's own list, not on the slice concatenation: nothing
    // rewrites KWin's order when a verb reorders a slice, so the two can
    // disagree, and a position summed from slice sizes then lands the desktop
    // next to some other screen's workspace. The slot's neighbour in the
    // slice says where KWin really has that part of the slice.
    const QList<WorkspaceEntry> entries = m_map.slice(screenId);
    const int slot = qBound(0, sliceIndex, static_cast<int>(entries.size()));
    int position = -1;
    for (int i = slot - 1; i >= 0 && position < 0; --i) {
        const int index = static_cast<int>(m_lastIds.indexOf(entries.at(i).desktopId));
        if (index >= 0) {
            position = index + 1;
        }
    }
    for (int i = slot; i < entries.size() && position < 0; ++i) {
        const int index = static_cast<int>(m_lastIds.indexOf(entries.at(i).desktopId));
        if (index >= 0) {
            position = index;
        }
    }
    if (position < 0) {
        position = static_cast<int>(m_map.globalPositionForInsert(screenId, slot));
    }
    // KWin applies what we asked for earlier before this create. Removes it
    // has applied (or will) ahead of the anchor pull it down, and creates
    // ahead of it push it up. Creates are counted whether or not they echoed,
    // since an echoed one is in KWin's list but not yet in m_lastIds.
    int removedAhead = 0;
    QList<int> createsAt;
    for (const PendingOp& op : m_ledger) {
        if (op.kind == PendingOp::Kind::Remove) {
            const int index = static_cast<int>(m_lastIds.indexOf(op.desktopId));
            if (index >= 0 && index < position) {
                ++removedAhead;
            }
        } else if (op.kind == PendingOp::Kind::Create) {
            createsAt.append(op.globalPosition);
        }
    }
    position -= removedAhead;
    std::sort(createsAt.begin(), createsAt.end());
    for (const int at : std::as_const(createsAt)) {
        if (at <= position) {
            ++position;
        }
    }
    return static_cast<uint>(qMax(0, position));
}

bool WorkspaceReconciler::maintainScreen(const QString& screenId)
{
    if (!m_adopted) {
        return false;
    }
    const auto entries = m_map.slice(screenId);

    // A screen must always hold at least one desktop. This one is NOT
    // cap-gated: at the ceiling a screen with an empty slice would otherwise
    // get no desktop at all and no retry, so take one from a donor screen that
    // can spare it rather than asking KWin for a desktop it will refuse.
    if (entries.isEmpty()) {
        if (hasPendingCreate(screenId)) {
            return false;
        }
        // Donor transfer covers TWO refusals, not just the cap. A spent create
        // budget means KWin is not answering this screen's requests, and a
        // transfer needs no answer from KWin at all.
        const bool createRefused = m_createRefusals.value(screenId, 0) >= MaxCreateRefusals;
        if (knownDesktopCount() + pendingCreateCount() >= m_desktopCap || createRefused) {
            const QStringList order = m_map.screenOrder();
            for (const QString& donor : order) {
                if (donor == screenId || m_map.sliceSize(donor) <= 1) {
                    continue;
                }
                const QString spare = trailingEmptyOf(donor);
                if (spare.isEmpty()) {
                    continue;
                }
                qCWarning(lcWorkspaceRec)
                    << (createRefused ? "create budget spent;" : "at the desktop cap;") << "transferring" << spare
                    << "from" << donor << "to" << screenId << "so no screen is left without a workspace";
                m_map.transfer(spare, screenId, 0);
                maintainScreen(donor);
                // The donor may have been showing its trailing empty, and the
                // receiver may be sitting on someone else's desktop.
                evaluateForeign(donor);
                evaluateForeign(screenId);
                return true;
            }
            qCWarning(lcWorkspaceRec) << "cannot give screen" << screenId << "a workspace:"
                                      << (createRefused ? "its create budget is spent" : "at the desktop cap")
                                      << "and no donor screen can spare one; it holds no workspace";
            return false;
        }
        requestCreateAt(screenId, -1, QString());
        return false;
    }

    // Trailing empty: exactly one empty dynamic desktop at the end.
    if (trailingEmptyOf(screenId).isEmpty()) {
        if (!hasPendingCreate(screenId)) {
            requestCreateAt(screenId, -1, QString());
        }
        return false;
    }

    // Surplus empties: destroy from the second-to-last inward. The scan does
    // NOT stop at the first occupied desktop: an explicit named-workspace
    // position can land a create mid-slice and leave an empty dynamic desktop
    // behind it. isReapable keeps named, displaced and shown ones.
    for (int i = entries.size() - 2; i >= 0; --i) {
        if (isReapable(entries.at(i).desktopId)) {
            scheduleDestroyCheck(entries.at(i).desktopId);
        }
    }
    return false;
}

bool WorkspaceReconciler::maintainInvariants()
{
    bool changed = false;
    const QStringList order = m_map.screenOrder();
    for (const QString& screenId : order) {
        changed = maintainScreen(screenId) || changed;
    }
    return changed;
}

} // namespace PhosphorWorkspaces
