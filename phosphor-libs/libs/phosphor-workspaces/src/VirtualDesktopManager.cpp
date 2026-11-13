// SPDX-FileCopyrightText: 2026 fuddlesworth
// SPDX-License-Identifier: LGPL-2.1-or-later

#include <PhosphorWorkspaces/VirtualDesktopManager.h>

#include <PhosphorIdentity/VirtualScreenId.h>

#include <QDBusArgument>
#include <QDBusConnection>
#include <QDBusInterface>
#include <QDBusMessage>
#include <QDBusPendingCall>
#include <QDBusPendingCallWatcher>
#include <QDBusPendingReply>
#include <QDBusVariant>
#include <QSet>
#include <QTimer>

#include <algorithm>
#include <array>
#include <limits>
#include <memory>
#include <utility>

namespace PhosphorWorkspaces {

namespace {

/// Placeholder label for a desktop KWin gave no name to.
///
/// Deliberately untranslated. `PhosphorI18n::tr()` — the project's only
/// sanctioned translation entry point for C++ — lives in `plasmazones/src/phosphor_i18n.h`,
/// inside the GPL-3.0 application tree. This library is LGPL-2.1 precisely so
/// third-party tools can link it without inheriting GPL, so reaching into that
/// header would defeat the licence split the project maintains on purpose.
/// The shell tier that renders these labels has no translation wiring either,
/// so the string localises when that story lands, together with the rest of
/// the shell's text. Same reasoning as the bar's widget display names.
QString fallbackDesktopName(int oneBasedPosition)
{
    return QStringLiteral("Desktop %1").arg(oneBasedPosition);
}

QString kwinService()
{
    return QStringLiteral("org.kde.KWin");
}

QString kwinVdmPath()
{
    return QStringLiteral("/VirtualDesktopManager");
}

QString kwinVdmInterface()
{
    return QStringLiteral("org.kde.KWin.VirtualDesktopManager");
}

/// Bound on every call this class makes to KWin. Far beyond any healthy round
/// trip, and short enough that a wedged-but-registered KWin cannot stall a
/// caller or hold the one list read in flight for long.
constexpr int KWinCallTimeoutMs = 1000;
/// A failed list read is retried this many times, this far apart, before the
/// change it was reading is given up (retryOrAbandonRefresh).
constexpr int MaxRefreshRetries = 3;
constexpr int RefreshRetryDelayMs = 500;

/// Read desktopCreated's or desktopDataChanged's s(iss) payload. The struct's
/// position is `u` on the wire (KWin's DBusDesktopDataStruct holds a uint,
/// whatever its introspection XML says), so it is read as one. False for a
/// message that does not carry a well-formed payload for a non-empty id.
bool parseDesktopData(const QDBusMessage& message, QString& desktopId, int& position, QString& name)
{
    const QList<QVariant> args = message.arguments();
    if (args.size() < 2 || args.at(1).userType() != qMetaTypeId<QDBusArgument>()) {
        return false;
    }
    desktopId = args.at(0).toString();
    const QDBusArgument data = args.at(1).value<QDBusArgument>();
    uint wirePosition = 0;
    QString dataId;
    data.beginStructure();
    data >> wirePosition >> dataId >> name;
    data.endStructure();
    if (desktopId.isEmpty() || dataId != desktopId
        || wirePosition > static_cast<uint>(std::numeric_limits<int>::max())) {
        return false;
    }
    position = static_cast<int>(wirePosition);
    return true;
}

} // namespace

VirtualDesktopManager::VirtualDesktopManager(QObject* parent)
    : QObject(parent)
{
}

VirtualDesktopManager::~VirtualDesktopManager()
{
    stop();
}

bool VirtualDesktopManager::init()
{
    initKWinDBus();
    return true;
}

void VirtualDesktopManager::initKWinDBus()
{
    // Re-entry guard: a second init() would otherwise construct a second
    // QDBusInterface (leaking the first as a child for the object's
    // lifetime) and double every session-bus subscription, so each KWin
    // signal would be handled twice while stop() removed only one hook
    // per pair.
    if (m_kwinVDInterface) {
        return;
    }
    // Owned here until KWin answered, so a failed probe is freed without a
    // manual delete and leaves m_kwinVDInterface null for a later init().
    auto kwinInterface = std::make_unique<QDBusInterface>(kwinService(), kwinVdmPath(), kwinVdmInterface(),
                                                          QDBusConnection::sessionBus());
    if (!kwinInterface->isValid()) {
        return;
    }
    kwinInterface->setParent(this);
    m_kwinVDInterface = kwinInterface.release();
    m_useKWinDBus = true;
    // Bounds the calls made THROUGH the interface: the init-only blocking
    // `rows` / `current` property reads in refreshFromKWin and the
    // createDesktop / removeDesktop / setDesktopName method calls, which then
    // answer NoReply after a second instead of QDBusInterface's 25-second
    // default (createDesktopAnswered reports that as Unanswered). The list
    // reads and setCurrent go through the connection with the same bound
    // passed per call.
    //
    // This does NOT bound everything: QDBusInterface's constructor above
    // already performed a blocking Introspect call at the 25-second default
    // before isValid() could be consulted, and setTimeout cannot reach
    // backwards to it.
    m_kwinVDInterface->setTimeout(KWinCallTimeoutMs);

    refreshFromKWin();
    subscribeToKWin();
}

void VirtualDesktopManager::subscribeToKWin()
{
    // Guarded so a stop()/start() cycle re-subscribes exactly once; a
    // doubled subscription would handle every KWin signal twice while
    // stop() removed only one hook per pair.
    if (m_kwinSubscribed || !m_kwinVDInterface) {
        return;
    }
    m_kwinSubscribed = true;

    QDBusConnection bus = QDBusConnection::sessionBus();
    bus.connect(kwinService(), kwinVdmPath(), kwinVdmInterface(), QStringLiteral("currentChanged"), this,
                SLOT(onKWinCurrentChanged(QString)));
    bus.connect(kwinService(), kwinVdmPath(), kwinVdmInterface(), QStringLiteral("countChanged"), this,
                SLOT(onNumberOfDesktopsChanged(uint)));
    bus.connect(kwinService(), kwinVdmPath(), kwinVdmInterface(), QStringLiteral("desktopCreated"), this,
                SLOT(onKWinDesktopCreated(QDBusMessage)));
    // Renames and position moves. KWin leaves `count` untouched for
    // these, so without this subscription a stale desktop name would
    // survive until an unrelated create/remove forced a refresh.
    bus.connect(kwinService(), kwinVdmPath(), kwinVdmInterface(), QStringLiteral("desktopDataChanged"), this,
                SLOT(onKWinDesktopDataChanged(QDBusMessage)));
    bus.connect(kwinService(), kwinVdmPath(), kwinVdmInterface(), QStringLiteral("desktopRemoved"), this,
                SLOT(onKWinDesktopRemoved(QString)));
    // A live grid reshape (e.g. 1×4 → 2×2) changes `rows` WITHOUT changing
    // the desktop count, so it fires neither countChanged nor created/removed
    // — without this the cached row count goes stale and cross-desktop
    // directional navigation computes neighbours against the wrong grid shape.
    bus.connect(kwinService(), kwinVdmPath(), kwinVdmInterface(), QStringLiteral("rowsChanged"), this,
                SLOT(onKWinDesktopRowsChanged(uint)));
}

void VirtualDesktopManager::applyDesktopListReply(const QDBusMessage& reply, const QString& currentId, int rows)
{
    // Validate the Get(desktops) reply envelope, then hand the payload to
    // the shared core. A failed reply leaves the previous snapshot untouched
    // (commitDesktopList).
    if (reply.type() != QDBusMessage::ReplyMessage || reply.arguments().isEmpty()) {
        return;
    }
    const QDBusVariant dbusVariant = reply.arguments().at(0).value<QDBusVariant>();
    const QVariant innerVariant = dbusVariant.variant();
    if (innerVariant.userType() != qMetaTypeId<QDBusArgument>()) {
        return;
    }
    applyDesktopListArg(*static_cast<const QDBusArgument*>(innerVariant.constData()), currentId, rows);
}

bool VirtualDesktopManager::applyDesktopListArg(const QDBusArgument& arg, const QString& currentId, int rows)
{
    // `position` is read as int even though the WIRE type is a(uss).
    // KWin's introspection XML advertises a(iss), but `busctl get-property`
    // shows the marshalled value is actually a(uss). Reading it into an int
    // is bit-compatible for the non-negative positions KWin emits, and
    // QDBusDemarshaller's union read yields the right value — verified
    // against the live interface rather than inferred. Flagged here because
    // the XML and the wire disagree, which is exactly the kind of thing a
    // future reader would "fix" in the wrong direction.
    QList<DesktopEntry> desktops;
    arg.beginArray();
    while (!arg.atEnd()) {
        DesktopEntry entry;
        arg.beginStructure();
        arg >> entry.position >> entry.id >> entry.name;
        arg.endStructure();
        desktops.append(entry);
    }
    arg.endArray();
    return commitDesktopList(std::move(desktops), currentId, rows);
}

bool VirtualDesktopManager::commitDesktopList(QList<DesktopEntry> desktops, const QString& currentId, int rows)
{
    // Validate before touching a member, and commit nothing on failure. The
    // members must not be cleared up front: a failed reply (KWin restarting,
    // a congested bus, an async call that errored) would then leave ids empty
    // while the count kept its previous value, and the "ids and names are
    // index-aligned" contract this class publishes would be broken. Every
    // id-keyed operation fails silently in that state: the pager empties, and
    // both setCurrentDesktopById and setCurrentDesktop drop every switch
    // because they validate against the empty id list.
    //
    // An empty list is a well-formed reply, but KWin always has at least one
    // desktop, so it means the demarshal produced nothing usable. D-Bus
    // replies are a system boundary, and a signature mismatch makes
    // QDBusArgument extraction yield DEFAULTS silently, so an id that
    // demarshalled empty (never a real KWin UUID) or a repeated one rejects
    // the whole list too.
    if (desktops.isEmpty()) {
        return false;
    }
    QSet<QString> seen;
    seen.reserve(desktops.size());
    for (const auto& desktop : std::as_const(desktops)) {
        if (desktop.id.isEmpty() || seen.contains(desktop.id)) {
            return false;
        }
        seen.insert(desktop.id);
    }

    // Stable, so two entries reporting the same position keep the order KWin
    // listed them in.
    std::stable_sort(desktops.begin(), desktops.end(), [](const DesktopEntry& a, const DesktopEntry& b) {
        return a.position < b.position;
    });

    QStringList newIds;
    QStringList newNames;
    QStringList newRawNames;
    newIds.reserve(desktops.size());
    newNames.reserve(desktops.size());
    newRawNames.reserve(desktops.size());
    for (const auto& desktop : std::as_const(desktops)) {
        newIds.append(desktop.id);
        newRawNames.append(desktop.name);
        newNames.append(desktop.name.isEmpty() ? fallbackDesktopName(newIds.size()) : desktop.name);
    }

    // The count IS the list length. Reading it from the separate `count`
    // property would reintroduce the possibility of the two disagreeing.
    const int newCount = newIds.size();

    // How the POSITIONS moved, while the old list is still here to compare
    // against. Ids are stable across a renumber and positions are not, so this
    // is the only moment the answer exists — see desktopRemovedAt's doc for
    // why the count alone cannot supply it. One map for every shape of change
    // (F787). The first snapshot has no previous list and compares against the
    // previous session's, when the daemon handed one over.
    const bool liveChange = !m_desktopIds.isEmpty();
    const PhosphorEngine::DesktopRemap remap =
        PhosphorEngine::DesktopRemap::fromIds(liveChange ? m_desktopIds : m_baselineDesktopIds, newIds);
    m_baselineDesktopIds.clear();
    const int removedPosition = liveChange ? remap.singleRemoval() : 0;

    // The current desktop by its id. An id this list lacks (the current
    // desktop was removed and no currentChanged named its replacement) goes
    // where KWin sends a removed desktop's screens, its successor. Clamped
    // either way: a removal BEFORE the current one renumbers it downward
    // without KWin sending currentChanged (the id did not move), so this is
    // the only place an out-of-range position is guaranteed to be caught.
    int newCurrent = m_currentDesktop;
    const int currentIdx = currentId.isEmpty() ? -1 : newIds.indexOf(currentId);
    if (currentIdx >= 0) {
        newCurrent = currentIdx + 1;
    } else if (liveChange && !remap.isIdentity()) {
        newCurrent = remap.successor(m_currentDesktop);
    }
    newCurrent = qBound(1, newCurrent, qMax(1, newCount));

    const bool listChanged = (newIds != m_desktopIds || newNames != m_desktopNames || newRawNames != m_rawDesktopNames);
    const bool idsChanged = (newIds != m_desktopIds);
    const bool currentChanged = (newCurrent != m_currentDesktop);
    const bool countChanged = (newCount != m_desktopCount);

    // Renames: a desktop on both sides of the commit whose raw name moved.
    QList<std::pair<QString, QString>> renamed;
    if (liveChange) {
        for (int i = 0; i < newIds.size(); ++i) {
            const int oldIdx = m_desktopIds.indexOf(newIds.at(i));
            if (oldIdx >= 0 && oldIdx < m_rawDesktopNames.size() && m_rawDesktopNames.at(oldIdx) != newRawNames.at(i)) {
                renamed.append({newIds.at(i), newRawNames.at(i)});
            }
        }
    }

    // This commit ends the change: the next KWin signal is a new one.
    const bool changeWasPending = m_desktopListChangePending;
    m_desktopListChangePending = false;
    m_pendingCurrentId.clear();

    m_desktopIds = newIds;
    m_desktopNames = newNames;
    m_rawDesktopNames = newRawNames;
    m_desktopCount = newCount;
    m_desktopRows = qMax(1, rows);
    m_currentDesktop = newCurrent;
    m_liveIds = newIds;
    m_liveRawNames.clear();
    for (int i = 0; i < newIds.size(); ++i) {
        m_liveRawNames.insert(newIds.at(i), newRawNames.at(i));
    }

    // This manager's OWN per-screen mirror moves through the same map, and
    // BEFORE any signal goes out: a consumer of desktopListChanged reads the
    // per-screen desktops against the new list (F111). A screen showing a
    // removed desktop takes its successor, as KWin moves it there, and
    // anything past the new count is pulled down with it, so
    // clampScreenDesktopsToCount below finds nothing to correct.
    //
    // SILENTLY, with no screenDesktopChanged, and the reason is worth keeping:
    // that signal means "this output switched desktops" and its subscriber
    // runs the whole context-switch pass — cancelling drag previews,
    // re-pinning sticky screens, pushing the desktop into all three engines,
    // re-announcing the screen set and showing the switch OSD. Emitting it
    // here would run that pass against engine state that has NOT been
    // renumbered yet, since the renumber signals go out below, and the push it
    // makes would then be moved a SECOND time by the engines' own renumber.
    // Nothing switched desktops; the desktops were renumbered underneath
    // everyone, which is what those signals say.
    //
    // A first snapshot moves no screen: the map then compares against the
    // previous session, and every screen this manager knows of was reported
    // in the live numbering.
    if (liveChange && !remap.isIdentity()) {
        for (auto it = m_screenDesktops.begin(); it != m_screenDesktops.end(); ++it) {
            it.value() = qBound(1, remap.successor(it.value()), m_desktopCount);
        }
    }

    // The settled id list FIRST, ahead of every signal below that carries a
    // desktop NUMBER (currentDesktopChanged, desktopsRenumbered, the count and
    // anything their subscribers re-report). The dynamic-workspaces reconciler
    // resolves numbers against the id list it last settled: announcing the
    // list after them would have it resolve each against the previous list.
    if (idsChanged) {
        Q_EMIT desktopListChanged(m_desktopIds);
    }
    // Change-gated: a refresh runs on create, remove, and every rename, and
    // most of those leave this data untouched. An unconditional emit would
    // reset a pager's model on unrelated events.
    if (listChanged) {
        Q_EMIT desktopsChanged();
    }
    for (const auto& [desktopId, name] : std::as_const(renamed)) {
        Q_EMIT desktopRenamed(desktopId, name);
    }
    // A desktop added or removed BEFORE the current one renumbers the current
    // position while its id stays put, so KWin sends no currentChanged and
    // this is the only place the move is observable. Consumers key placement
    // state on the 1-based number and re-read only on this signal.
    if (currentChanged) {
        Q_EMIT currentDesktopChanged(m_currentDesktop);
    }
    // BEFORE the count, deliberately. A consumer keyed on positions has to
    // drop the removed position's state and shift the rest down before
    // anything reasons about the new count — and once it has, the count
    // handler's "prune everything past newCount" sweep correctly finds
    // nothing left to do.
    if (!remap.isIdentity()) {
        Q_EMIT desktopsRenumbered(remap);
        if (removedPosition > 0) {
            Q_EMIT desktopRemovedAt(removedPosition);
        }
    }
    // The count notification belongs HERE, where the value is committed. The
    // one count committed ahead of the list is a growing one from the
    // list-less fallback in onNumberOfDesktopsChanged, and this finds it
    // already equal.
    if (countChanged) {
        clampScreenDesktopsToCount();
        Q_EMIT desktopCountChanged(m_desktopCount);
    }
    if (changeWasPending) {
        Q_EMIT desktopListChangeSettled();
    }
    return true;
}

void VirtualDesktopManager::refreshFromKWin()
{
    if (!m_kwinVDInterface || !m_kwinVDInterface->isValid()) {
        return;
    }

    if (!m_running) {
        // Bump so an asynchronous read still outstanding from before a stop()
        // cannot land its older list on top of this one, and forget it.
        ++m_refreshGeneration;
        m_refreshInFlight = false;
        // The blocking path (before start() or after stop()) reads the scalar properties
        // into LOCALS and hands them to applyDesktopListReply, which commits
        // them together with the desktop list or commits nothing at all —
        // committing here would leave a fresh `count` beside stale `ids` on
        // a dropped reply. Rows clamps to >= 1 so a missing or zero property
        // cannot divide the grid arithmetic by zero.
        const QVariant rowsVar = m_kwinVDInterface->property("rows");
        const int pendingRows = rowsVar.isValid() ? qMax(1, rowsVar.toInt()) : m_desktopRows;

        const QVariant currentVar = m_kwinVDInterface->property("current");
        QString currentId;
        if (currentVar.isValid()) {
            currentId = currentVar.toString();
        }

        QDBusMessage getDesktopsMsg = QDBusMessage::createMethodCall(
            kwinService(), kwinVdmPath(), QStringLiteral("org.freedesktop.DBus.Properties"), QStringLiteral("Get"));
        getDesktopsMsg << kwinVdmInterface() << QStringLiteral("desktops");

        QDBusMessage reply = QDBusConnection::sessionBus().call(getDesktopsMsg, QDBus::Block, KWinCallTimeoutMs);
        applyDesktopListReply(reply, currentId, pendingRows);
        return;
    }

    // One read at a time (F914). KWin sends its desktop signals and its reply
    // on one connection, in the order it produced them, and handles the read
    // whole between two changes, so the reply describes KWin after every
    // signal this manager received before the reply. A signal arriving while
    // a read is in flight is therefore covered by it, and a second read would
    // only repeat it; a removal alone sends one desktopDataChanged per shifted
    // desktop plus desktopRemoved and countChanged, each of which used to
    // issue its own read.
    if (m_refreshInFlight) {
        return;
    }
    m_refreshInFlight = true;
    ++m_refreshGeneration;
    const uint thisGeneration = m_refreshGeneration;

    // ONE async GetAll for rows + current + desktops, so the whole snapshot
    // lands in a single round trip without blocking the GUI thread.
    QDBusMessage getAllMsg = QDBusMessage::createMethodCall(
        kwinService(), kwinVdmPath(), QStringLiteral("org.freedesktop.DBus.Properties"), QStringLiteral("GetAll"));
    getAllMsg << kwinVdmInterface();

    QDBusPendingCall pendingCall = QDBusConnection::sessionBus().asyncCall(getAllMsg, KWinCallTimeoutMs);
    auto* watcher = new QDBusPendingCallWatcher(pendingCall, this);

    connect(watcher, &QDBusPendingCallWatcher::finished, this, [this, thisGeneration](QDBusPendingCallWatcher* w) {
        w->deleteLater();
        if (thisGeneration != m_refreshGeneration) {
            return;
        }
        m_refreshInFlight = false;

        QDBusPendingReply<QVariantMap> reply = *w;
        if (reply.isError()) {
            retryOrAbandonRefresh();
            return;
        }
        const QVariantMap props = reply.value();

        const QVariant rowsVar = props.value(QStringLiteral("rows"));
        const int pendingRows = rowsVar.isValid() ? qMax(1, rowsVar.toInt()) : m_desktopRows;
        const QString currentId = props.value(QStringLiteral("current")).toString();

        // In a GetAll a{sv} map the complex-typed `desktops` value arrives
        // as a QDBusArgument inside the QVariant; the scalar members above
        // demarshal to plain types.
        const QVariant desktopsVar = props.value(QStringLiteral("desktops"));
        if (desktopsVar.userType() != qMetaTypeId<QDBusArgument>()
            || !applyDesktopListArg(*static_cast<const QDBusArgument*>(desktopsVar.constData()), currentId,
                                    pendingRows)) {
            retryOrAbandonRefresh();
            return;
        }
        m_refreshRetries = 0;
    });
}

void VirtualDesktopManager::retryOrAbandonRefresh()
{
    if (m_refreshRetries < MaxRefreshRetries) {
        ++m_refreshRetries;
        QTimer::singleShot(RefreshRetryDelayMs, this, [this]() {
            if (m_running) {
                refreshFromKWin();
            }
        });
        return;
    }
    m_refreshRetries = 0;
    // KWin answered nothing usable several times over. Give the change up
    // rather than hold it open: a count committed ahead of its list (the
    // fallback in onNumberOfDesktopsChanged) goes back to the list this
    // manager actually has, so desktopCount() and desktopIds() agree again,
    // and a pending change is settled so the stores that marked their entries
    // for it stop waiting. The next KWin signal starts over.
    m_liveIds = m_desktopIds;
    m_pendingCurrentId.clear();
    if (!m_desktopIds.isEmpty() && m_desktopCount != m_desktopIds.size()) {
        m_desktopCount = m_desktopIds.size();
        if (m_currentDesktop > m_desktopCount) {
            m_currentDesktop = m_desktopCount;
            Q_EMIT currentDesktopChanged(m_currentDesktop);
        }
        clampScreenDesktopsToCount();
        Q_EMIT desktopCountChanged(m_desktopCount);
    }
    if (m_desktopListChangePending) {
        m_desktopListChangePending = false;
        Q_EMIT desktopListChangeSettled();
    }
}

void VirtualDesktopManager::onKWinCurrentChanged(const QString& desktopId)
{
    // While a change is pending this manager's list is the OLD one and
    // KWin's id is already numbered by the new one (a removal moves the active
    // output off the removed desktop mid-change). Resolving it now would
    // publish the id's old position. The commit that ends the change numbers
    // it instead, and the read in flight (or this one) carries `current` too
    // (F950).
    if (m_desktopListChangePending) {
        m_pendingCurrentId = desktopId;
        refreshFromKWin();
        return;
    }
    const int idx = m_desktopIds.indexOf(desktopId);
    if (idx < 0) {
        // An id this snapshot has not seen yet. Publishing a fallback of 1
        // would hand every consumer a wrong position until the refresh
        // corrected it — daemon placement keys on this number. Re-read
        // instead; the commit reads `current` too, so nothing is lost.
        refreshFromKWin();
        return;
    }
    const int newDesktop = idx + 1;

    if (m_currentDesktop == newDesktop) {
        return;
    }

    m_currentDesktop = newDesktop;
    Q_EMIT currentDesktopChanged(m_currentDesktop);
}

void VirtualDesktopManager::onKWinDesktopCreated(const QDBusMessage& message)
{
    QString desktopId;
    int position = 0;
    QString name;
    if (!parseDesktopData(message, desktopId, position, name)) {
        // Still a change: re-read the list rather than lose it. The echo goes
        // out whenever the id itself is readable, position and name unknown.
        // KWin emits the signal before it answers the createDesktop call, so
        // an answer with no echo ahead of it reads as KWin refusing the
        // create at its ceiling, and a payload this side could not parse is
        // no such refusal. The live list is left alone without the position,
        // so the read below, not countChanged, commits this change.
        const QList<QVariant> args = message.arguments();
        const QString echoedId =
            (!args.isEmpty() && args.at(0).userType() == QMetaType::QString) ? args.at(0).toString() : QString();
        if (!echoedId.isEmpty()) {
            noteDesktopListChanging();
            Q_EMIT kwinDesktopCreated(echoedId);
        }
        refreshFromKWin();
        return;
    }
    ingestDesktopCreated(desktopId, position, name);
}

void VirtualDesktopManager::ingestDesktopCreated(const QString& desktopId, int position, const QString& name)
{
    // The live list before the echo: the reconciler matches its ledger on the
    // echo and settles on desktopListChanged, which this change's countChanged
    // brings (onNumberOfDesktopsChanged).
    noteDesktopListChanging();
    if (!m_liveIds.isEmpty()) {
        m_liveIds.removeAll(desktopId);
        m_liveIds.insert(qBound(0, position, int(m_liveIds.size())), desktopId);
        m_liveRawNames.insert(desktopId, name);
    }
    Q_EMIT kwinDesktopCreated(desktopId);
    refreshFromKWin();
}

void VirtualDesktopManager::onKWinDesktopRemoved(const QString& desktopId)
{
    noteDesktopListChanging();
    m_liveIds.removeAll(desktopId);
    m_liveRawNames.remove(desktopId);
    Q_EMIT kwinDesktopRemoved(desktopId);
    refreshFromKWin();
}

void VirtualDesktopManager::onKWinDesktopRowsChanged(uint rows)
{
    // The signal carries the new row count, so no list read is needed: the
    // count is untouched by a reshape, and desktopRows() is pulled on demand.
    m_desktopRows = qMax(1, static_cast<int>(qMin(rows, static_cast<uint>(std::numeric_limits<int>::max()))));
}

void VirtualDesktopManager::onKWinDesktopDataChanged(const QDBusMessage& message)
{
    QString desktopId;
    int position = 0;
    QString name;
    if (!parseDesktopData(message, desktopId, position, name)) {
        refreshFromKWin();
        return;
    }
    ingestDesktopDataChanged(desktopId, position, name);
}

void VirtualDesktopManager::ingestDesktopDataChanged(const QString& desktopId, int position, const QString& name)
{
    // KWin sends this for a rename and for every desktop a create, a remove
    // or a move shifts, each carrying the desktop's NEW position. Moving the
    // desktop to that position in the live list, in the order KWin sends
    // them, ends at KWin's list once the change is through: the shifts go out
    // in ascending position, so each lands its desktop where the next one
    // cannot displace it. A rename leaves the position as it is and starts no
    // change: it renumbers nothing, and the read below commits the name.
    // Without a trusted live list (after stop()) any of these may be a move.
    if (m_liveIds.isEmpty()) {
        noteDesktopListChanging();
    } else {
        m_liveRawNames.insert(desktopId, name);
        const int at = m_liveIds.indexOf(desktopId);
        if (at != position) {
            noteDesktopListChanging();
            if (at >= 0) {
                m_liveIds.removeAt(at);
            }
            m_liveIds.insert(qBound(0, position, int(m_liveIds.size())), desktopId);
        }
    }
    refreshFromKWin();
}

void VirtualDesktopManager::start()
{
    if (m_running) {
        return;
    }

    m_running = true;

    if (m_useKWinDBus) {
        // Re-install the session-bus subscriptions dropped by a prior
        // stop(). subscribeToKWin is guarded, so on first start (where
        // init() already subscribed) this is a no-op rather than a
        // doubling.
        subscribeToKWin();
        refreshFromKWin();
    }
}

void VirtualDesktopManager::stop()
{
    m_running = false;
    // Drop the session-bus subscriptions too. Clearing the flag alone left a
    // "stopped" manager still servicing currentChanged / countChanged /
    // desktopCreated / desktopRemoved / desktopDataChanged, and each of
    // those re-enters refreshFromKWin and issues blocking D-Bus calls — so
    // stop() did not stop anything, it only changed which code path the
    // work took. It matters most during destruction: ~VirtualDesktopManager
    // calls stop(), and a signal dispatched between there and ~QObject would
    // re-enter a half-destroyed object.
    //
    // Each (signal, slot) pair must be named EXPLICITLY. QDBusConnection
    // registers hooks keyed on signal name plus path, and its disconnect()
    // returns false for a null slot or an empty signal name — a wildcard
    // "drop everything for this interface" call compiles, runs, and removes
    // nothing at all. Mirrors the connect() calls in subscribeToKWin().
    // Not constexpr: SLOT() expands to a qFlagLocation() call.
    const std::array subscriptions{
        std::pair<const char*, const char*>{"currentChanged", SLOT(onKWinCurrentChanged(QString))},
        std::pair<const char*, const char*>{"countChanged", SLOT(onNumberOfDesktopsChanged(uint))},
        std::pair<const char*, const char*>{"desktopCreated", SLOT(onKWinDesktopCreated(QDBusMessage))},
        std::pair<const char*, const char*>{"desktopDataChanged", SLOT(onKWinDesktopDataChanged(QDBusMessage))},
        std::pair<const char*, const char*>{"desktopRemoved", SLOT(onKWinDesktopRemoved(QString))},
        std::pair<const char*, const char*>{"rowsChanged", SLOT(onKWinDesktopRowsChanged(uint))},
    };
    for (const auto& [signalName, slotSpec] : subscriptions) {
        QDBusConnection::sessionBus().disconnect(kwinService(), kwinVdmPath(), kwinVdmInterface(),
                                                 QLatin1String(signalName), this, slotSpec);
    }
    m_kwinSubscribed = false;
    // Signals stop arriving here, so the list built from them can no longer
    // be trusted to follow KWin: nothing is committed from it until a read
    // has committed KWin's own list again (start()).
    m_liveIds.clear();
    m_liveRawNames.clear();
}

int VirtualDesktopManager::currentDesktop() const
{
    return m_useKWinDBus ? m_currentDesktop : 1;
}

int VirtualDesktopManager::currentDesktopForScreen(const QString& screenId) const
{
    const auto it = m_screenDesktops.constFind(screenId);
    if (it != m_screenDesktops.constEnd()) {
        return it.value();
    }

    // The map is keyed by PHYSICAL output id (updateScreenDesktop is fed from
    // the effect's per-output report), but most callers ask with an EFFECTIVE
    // id, and a subdivided output only ever produces its `/vs:N` children. An
    // exact-key miss on a virtual id is therefore the normal case there, not an
    // unknown screen — resolve it against the parent output before falling back
    // to the global desktop, which would otherwise be wrong on every virtual
    // screen of a monitor running its own desktop.
    if (PhosphorIdentity::VirtualScreenId::isVirtual(screenId)) {
        const auto physical =
            m_screenDesktops.constFind(PhosphorIdentity::VirtualScreenId::extractPhysicalId(screenId));
        if (physical != m_screenDesktops.constEnd()) {
            return physical.value();
        }
    }

    return currentDesktop();
}

bool VirtualDesktopManager::perScreenModeActive() const
{
    if (m_screenDesktops.size() < 2) {
        return false;
    }
    auto it = m_screenDesktops.constBegin();
    const int first = it.value();
    for (++it; it != m_screenDesktops.constEnd(); ++it) {
        if (it.value() != first) {
            return true;
        }
    }
    return false;
}

void VirtualDesktopManager::updateScreenDesktop(const QString& screenId, int desktop)
{
    if (screenId.isEmpty() || desktop < 1) {
        return;
    }
    if (m_screenDesktops.value(screenId, -1) == desktop) {
        return;
    }
    m_screenDesktops.insert(screenId, desktop);
    Q_EMIT screenDesktopChanged(screenId, desktop);
}

void VirtualDesktopManager::removeScreenDesktop(const QString& screenId)
{
    m_screenDesktops.remove(screenId);
}

bool VirtualDesktopManager::hasScreenDesktopReport(const QString& screenId) const
{
    if (m_screenDesktops.contains(screenId)) {
        return true;
    }
    if (PhosphorIdentity::VirtualScreenId::isVirtual(screenId)) {
        return m_screenDesktops.contains(PhosphorIdentity::VirtualScreenId::extractPhysicalId(screenId));
    }
    return false;
}

void VirtualDesktopManager::renameScreen(const QString& oldId, const QString& newId)
{
    // No row is ever stored under an empty id (updateScreenDesktop and
    // seedScreenDesktop refuse one), so an empty oldId finds nothing below.
    if (newId.isEmpty() || oldId == newId) {
        return;
    }
    const auto from = m_screenDesktops.constFind(oldId);
    if (from == m_screenDesktops.constEnd()) {
        return; // nothing recorded under the dead id
    }
    const int desktop = from.value();
    m_screenDesktops.remove(oldId);
    // A live-id row already exists: it is a fresher report than the one we
    // would carry over, so it stands and the carried value is discarded. The
    // value under newId did not change, so nothing is emitted.
    if (m_screenDesktops.contains(newId)) {
        return;
    }
    m_screenDesktops.insert(newId, desktop);
    // A seed, not a switch: the output shows the desktop it showed, under a
    // new name, and the engines learn it under that name with no switch pass
    // and no OSD (F221).
    Q_EMIT screenDesktopSeeded(newId, desktop);
}

void VirtualDesktopManager::seedScreenDesktop(const QString& screenId, int desktop)
{
    if (screenId.isEmpty() || desktop < 1) {
        return;
    }
    m_screenDesktops.insert(screenId, desktop);
    Q_EMIT screenDesktopSeeded(screenId, desktop);
}

void VirtualDesktopManager::setBaselineDesktopIds(const QStringList& desktopIds)
{
    if (desktopIds.isEmpty()) {
        return;
    }
    if (m_desktopIds.isEmpty()) {
        // No snapshot yet: the first one compares against this.
        m_baselineDesktopIds = desktopIds;
        return;
    }
    // The snapshot is already here, so the stores it renumbers are keyed on
    // the baseline's positions until this goes out. The per-screen mirror is
    // not touched: every screen in it was reported in the live numbering.
    const PhosphorEngine::DesktopRemap remap = PhosphorEngine::DesktopRemap::fromIds(desktopIds, m_desktopIds);
    if (!remap.isIdentity()) {
        Q_EMIT desktopsRenumbered(remap);
    }
}

void VirtualDesktopManager::clampScreenDesktopsToCount()
{
    // Clamp only entries above the live count: KWin renumbers on desktop
    // removal, so a screen pinned past the new count is pulled down to it here.
    // An entry whose desktop was RENUMBERED (a removal, an insert, a move) is
    // not this function's business: commitDesktopList moves those itself
    // through the position map, before this runs, and pulls down the ones a
    // removal at the END left out of range. What is left here is a count
    // committed with no list behind it: onNumberOfDesktopsChanged without a
    // list, and retryOrAbandonRefresh rolling a count back.
    //
    // Mutate first, then emit the captured value — emitting mid-iteration could
    // re-enter updateScreenDesktop and invalidate the hash iterator, and a
    // re-entrant write must not change the value we report for this clamp.
    QList<std::pair<QString, int>> clamped;
    for (auto it = m_screenDesktops.begin(); it != m_screenDesktops.end(); ++it) {
        if (it.value() > m_desktopCount) {
            it.value() = m_desktopCount;
            clamped.append({it.key(), m_desktopCount});
        }
    }
    for (const auto& [screenId, desktop] : clamped) {
        Q_EMIT screenDesktopChanged(screenId, desktop);
    }
}

void VirtualDesktopManager::noteDesktopListChanging()
{
    // Once per change (F1023). KWin sends several signals for one change (a
    // removal sends desktopDataChanged per shifted desktop, desktopRemoved and
    // countChanged), and the effect's pushes in the new numbering can arrive
    // between them. The window registry snapshots its records on this signal
    // and drops each one pushed since, so a second send re-armed records
    // already in the new numbering and the renumber moved them twice. The
    // commit ending the change clears the latch, so the NEXT change is
    // announced on its own even when it follows before a list read landed: a
    // record pushed between two changes is in the first one's numbering and
    // must be moved by the second (F466). Before the first snapshot there is
    // nothing live to renumber: that map compares against the previous session.
    if (!m_desktopListChangePending && !m_desktopIds.isEmpty()) {
        m_desktopListChangePending = true;
        Q_EMIT desktopListChanging();
    }
}

bool VirtualDesktopManager::liveListMatches(int count) const
{
    if (m_liveIds.size() != count || m_liveIds.isEmpty()) {
        return false;
    }
    const QSet<QString> unique(m_liveIds.cbegin(), m_liveIds.cend());
    return unique.size() == m_liveIds.size();
}

void VirtualDesktopManager::commitLiveList()
{
    QList<DesktopEntry> desktops;
    desktops.reserve(m_liveIds.size());
    for (int i = 0; i < m_liveIds.size(); ++i) {
        const QString& desktopId = m_liveIds.at(i);
        desktops.append(DesktopEntry{i, desktopId, m_liveRawNames.value(desktopId)});
    }
    // The current desktop's id: the one KWin named during the change, else
    // the one this manager had (a removed one resolves to its successor).
    const QString currentId = m_pendingCurrentId.isEmpty() ? desktopIdAt(m_currentDesktop) : m_pendingCurrentId;
    commitDesktopList(std::move(desktops), currentId, m_desktopRows);
}

void VirtualDesktopManager::setCurrentDesktop(int desktop)
{
    if (desktop < 1 || desktop > m_desktopCount) {
        return;
    }
    setCurrentDesktopById(desktopIdAt(desktop));
}

void VirtualDesktopManager::setCurrentDesktopById(const QString& desktopId)
{
    if (desktopId.isEmpty() || !m_useKWinDBus || !m_kwinVDInterface) {
        return;
    }
    // Only ids we actually know about. Writing an unknown UUID would
    // either be rejected by KWin or, worse, switch to a desktop this
    // manager has no record of, leaving m_currentDesktop wrong until the
    // next refresh.
    if (!m_desktopIds.contains(desktopId)) {
        return;
    }
    // Properties.Set, sent without waiting for the reply. QDBusInterface's
    // setProperty blocks the calling thread (the shell's GUI thread, for a
    // pager) until KWin answers; the switch itself arrives as currentChanged
    // either way (F706).
    QDBusMessage setCurrent = QDBusMessage::createMethodCall(
        kwinService(), kwinVdmPath(), QStringLiteral("org.freedesktop.DBus.Properties"), QStringLiteral("Set"));
    setCurrent << kwinVdmInterface() << QStringLiteral("current") << QVariant::fromValue(QDBusVariant(desktopId));
    QDBusConnection::sessionBus().send(setCurrent);
}

void VirtualDesktopManager::onNumberOfDesktopsChanged(uint count)
{
    // The parameter must stay `uint`: KWin's countChanged carries
    // `u` on the wire, and QDBusConnection::connect matches on the signature,
    // so an `int` slot here returns true at connect time and then never fires.
    // `uint` is therefore a wire type only — narrow once at this boundary and
    // let everything below stay int, matching m_currentDesktop, the 1-based
    // desktop math, and desktopCountChanged(int).
    // Boundary validation: KWin never emits 0, but this is a session-bus
    // signal any peer can send, and committing 0 would publish an invalid
    // 1-based currentDesktopChanged(0) below until a refresh corrected it.
    // The upper bound keeps the narrowing cast below well-defined.
    if (count < 1 || count > static_cast<uint>(std::numeric_limits<int>::max())) {
        return;
    }
    const int newCount = static_cast<int>(count);

    // KWin's countChanged closes a create, a remove or a resize: every
    // desktop signal of that change came before it, so the live list is
    // KWin's list now. Committing it here, rather than when a list read lands,
    // puts the renumber, the list and the count out together and ahead of
    // every report KWin sends after the change (Move to New Desktop's report
    // of desktop N+1 among them, F1022), and ends the change before the next
    // one can start (F466). The read already in flight still commits the
    // names and rows, and finds the list unchanged.
    if (m_desktopListChangePending && liveListMatches(newCount)) {
        commitLiveList();
        return;
    }
    if (m_desktopCount == newCount) {
        return;
    }

    // The signals did not add up to a list of this count (one was missed or
    // malformed), so the list read decides. A SHRINKING count is not committed
    // here: the read commits the count, the clamp and both signals together,
    // the renumber BEFORE the count. Committing it first published it ahead of
    // the map that says which desktop went, so a mid-list removal had the last
    // desktop's per-screen entry clamped and then moved again by the map, and
    // the daemon's count handler pruned the engine states past the new count
    // that the map was about to move down.
    //
    // A GROWING count is committed at once (F1022). Nothing is clamped or
    // pruned by it, and KWin sends countChanged before anything that uses the
    // new desktop, so every desktop range check (screenDesktopChanged,
    // setWindowMetadata) would otherwise refuse the effect's report of desktop
    // N+1 against the old count. The effect sends that report once. An insert
    // before the last desktop still renumbers when the read lands, after this
    // count went out, and a read that never lands rolls it back
    // (retryOrAbandonRefresh).
    if (m_useKWinDBus && m_kwinVDInterface && m_kwinVDInterface->isValid()) {
        noteDesktopListChanging();
        if (newCount > m_desktopCount) {
            m_desktopCount = newCount;
            Q_EMIT desktopCountChanged(m_desktopCount);
        }
        refreshFromKWin();
        return;
    }

    m_desktopCount = newCount;

    // Clamp against the live member, not the signal arg, so this handler
    // stays self-consistent with desktopCount() at every point.
    if (m_currentDesktop > m_desktopCount) {
        m_currentDesktop = m_desktopCount;
        Q_EMIT currentDesktopChanged(m_currentDesktop);
    }

    clampScreenDesktopsToCount();

    // Report the live member, not the signal argument, for the same reason
    // the clamp above reads it: the member is what desktopCount() answers,
    // so emitting anything else would let a consumer's cached count
    // disagree with a follow-up read. Matches the emit in
    // commitDesktopList, which already sends m_desktopCount.
    Q_EMIT desktopCountChanged(m_desktopCount);
}

int VirtualDesktopManager::desktopCount() const
{
    return m_useKWinDBus ? m_desktopCount : 1;
}

int VirtualDesktopManager::desktopRows() const
{
    return m_useKWinDBus ? qMax(1, m_desktopRows) : 1;
}

QStringList VirtualDesktopManager::desktopNames() const
{
    if (m_useKWinDBus && !m_desktopNames.isEmpty()) {
        return m_desktopNames;
    }

    QStringList names;
    int count = desktopCount();
    for (int i = 1; i <= count; ++i) {
        names.append(fallbackDesktopName(i));
    }
    return names;
}

bool VirtualDesktopManager::isAvailable() const
{
    return m_useKWinDBus;
}

QStringList VirtualDesktopManager::desktopIds() const
{
    // No synthesised fallback, unlike desktopNames(): a made-up name is a
    // harmless placeholder, but a made-up id would be a key that matches
    // nothing in KWin, and setCurrentDesktopById would silently drop every
    // switch made against it. An empty list says "no stable ids here",
    // which a caller can act on.
    return m_desktopIds;
}

void VirtualDesktopManager::createDesktop(uint position, const QString& name)
{
    if (!m_useKWinDBus || !m_kwinVDInterface) {
        return;
    }
    // KWin's createDesktop returns nothing, and refuses past its desktop cap
    // by doing nothing, so the reply is the only way to tell that refusal
    // from a stall (F782). KWin emits desktopCreated before it returns, and
    // the signal and the reply share one ordered connection, so the answer is
    // handled after the echo it would have caused.
    auto* watcher = new QDBusPendingCallWatcher(
        m_kwinVDInterface->asyncCall(QStringLiteral("createDesktop"), position, name), this);
    connect(watcher, &QDBusPendingCallWatcher::finished, this, [this, position, name](QDBusPendingCallWatcher* w) {
        w->deleteLater();
        Q_EMIT createDesktopAnswered(position, name, w->isError() ? CallAnswer::Unanswered : CallAnswer::Answered);
    });
}

void VirtualDesktopManager::removeDesktop(const QString& desktopId)
{
    if (!m_useKWinDBus || !m_kwinVDInterface || desktopId.isEmpty()) {
        return;
    }
    // Same contract as createDesktop's answer: KWin refuses the last desktop
    // and an unknown id by doing nothing.
    auto* watcher =
        new QDBusPendingCallWatcher(m_kwinVDInterface->asyncCall(QStringLiteral("removeDesktop"), desktopId), this);
    connect(watcher, &QDBusPendingCallWatcher::finished, this, [this, desktopId](QDBusPendingCallWatcher* w) {
        w->deleteLater();
        Q_EMIT removeDesktopAnswered(desktopId, w->isError() ? CallAnswer::Unanswered : CallAnswer::Answered);
    });
}

void VirtualDesktopManager::setDesktopName(const QString& desktopId, const QString& name)
{
    if (!m_useKWinDBus || !m_kwinVDInterface || desktopId.isEmpty()) {
        return;
    }
    m_kwinVDInterface->asyncCall(QStringLiteral("setDesktopName"), desktopId, name);
}

QString VirtualDesktopManager::desktopIdAt(int desktop) const
{
    if (desktop < 1 || desktop > m_desktopIds.size()) {
        return QString();
    }
    return m_desktopIds.at(desktop - 1);
}

int VirtualDesktopManager::desktopIndexOf(const QString& desktopId) const
{
    return m_desktopIds.indexOf(desktopId) + 1;
}

QStringList VirtualDesktopManager::rawDesktopNames() const
{
    return m_useKWinDBus ? m_rawDesktopNames : QStringList();
}

} // namespace PhosphorWorkspaces
