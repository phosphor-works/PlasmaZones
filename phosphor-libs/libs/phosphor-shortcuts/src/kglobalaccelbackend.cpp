// SPDX-FileCopyrightText: 2026 fuddlesworth
// SPDX-License-Identifier: LGPL-2.1-or-later

#include "backends.h"
#include "shortcutslogging.h"

#include <KGlobalAccel>
#include <QAction>
#include <QCoreApplication>
#include <QDBusArgument>
#include <QDBusConnection>
#include <QDBusMessage>
#include <QDBusMetaType>
#include <QDBusPendingCall>
#include <QHash>

#include <optional>

namespace PhosphorShortcuts::detail {

/// Number of ints inside each (ai) struct of kglobalaccel's a(ai) wire type.
/// Every sequence the live service hands back is exactly this wide,
/// zero-padded (Meta+1 reads as [285212720, 0, 0, 0]), so this is the width a
/// receiver can rely on.
constexpr int DBusKeySequenceInts = 4;

/// Wire type for kglobalaccel's a(ai): each element is a STRUCT wrapping one
/// sequence's chord ints. Registered (not hand-marshalled) so QtDBus derives
/// the exact signature for setForeignShortcutKeys.
///
/// The writer always pads to DBusKeySequenceInts. KGlobalAccel's QKeySequence
/// demarshaller has read that many ints unconditionally rather than stopping
/// at the array's end, and a short array then makes it read past the last
/// element, which libdbus answers with an abort() in the RECEIVING process.
/// kglobalaccel runs in-process inside kwin_wayland on a Plasma session, so a
/// short array there does not fail the call, it takes the compositor down
/// with it. The service may run an older release than the one this library
/// was built against, so the padding stays whatever the current release's
/// reader does: a padded array is what every release itself sends.
///
/// Namespaced rather than left at global scope: Q_DECLARE_METATYPE and
/// qDBusRegisterMetaType both need external linkage, so an anonymous namespace
/// is not available, and a bare name in a library TU is an ODR hazard against
/// anything else that links this library. The stream operators live here too so
/// ADL finds them from the registration below.
struct DBusKeySequence
{
    QList<int> keys;
};

inline QDBusArgument& operator<<(QDBusArgument& arg, const DBusKeySequence& seq)
{
    arg.beginStructure();
    // Zero-pad to the fixed width rather than writing keys directly. Trimming
    // to the chord count is what a reader cannot survive, and an over-long
    // list would leave the reader mid-array at endStructure.
    QList<int> padded = seq.keys.mid(0, DBusKeySequenceInts);
    while (padded.size() < DBusKeySequenceInts) {
        padded.append(0);
    }
    arg << padded;
    arg.endStructure();
    return arg;
}

inline const QDBusArgument& operator>>(const QDBusArgument& arg, DBusKeySequence& seq)
{
    arg.beginStructure();
    arg >> seq.keys;
    arg.endStructure();
    return arg;
}

} // namespace PhosphorShortcuts::detail

Q_DECLARE_METATYPE(PhosphorShortcuts::detail::DBusKeySequence)

namespace PhosphorShortcuts {

namespace {

const QString kKGlobalAccelService = QStringLiteral("org.kde.kglobalaccel");
const QString kKGlobalAccelPath = QStringLiteral("/kglobalaccel");
const QString kKGlobalAccelInterface = QStringLiteral("org.kde.KGlobalAccel");
/// Every raw call here is a local round trip to kglobalacceld. The bound only
/// matters when the service is wedged.
constexpr int kRawCallTimeoutMs = 1000;

/// A method call on kglobalacceld that never starts the service. At logout the
/// compositor that hosts it is going away, and an activatable standalone
/// kglobalacceld would otherwise be spawned just to answer a teardown call.
/// With autostart off a missing service fails at once instead of timing out.
QDBusMessage kglobalaccelCall(const QString& method)
{
    QDBusMessage call =
        QDBusMessage::createMethodCall(kKGlobalAccelService, kKGlobalAccelPath, kKGlobalAccelInterface, method);
    call.setAutoStartService(false);
    return call;
}

/// The component unique name KGlobalAccel files this process's actions under:
/// registerShortcut() stamps it on every action as the componentName
/// property, and the raw calls below address the same component.
QString ownComponent()
{
    return QCoreApplication::applicationName();
}

} // namespace

// KGlobalAccel's C++ API trafficks exclusively in QAction*. We hide that
// behind the id-based IBackend interface by owning one QAction per id —
// created on registerShortcut(), objectName'd to the id (KGlobalAccel uses
// objectName as the persistent key on disk), and connected so QAction::triggered
// fans out to IBackend::activated().
//
// Component name: KGlobalAccel groups shortcuts by
// QCoreApplication::applicationName() at the time setShortcut() is first
// called. Callers should set an applicationName before instantiating this
// backend — otherwise shortcuts end up under an empty component which is
// hard to find in System Settings.
//
// Every connection this backend makes to one of its QActions uses the backend
// as the context, and every teardown cuts only those with
// action->disconnect(this). A bare action->disconnect() would also cut the
// KGlobalAccel client's own destroyed() handler (its context is the
// KGlobalAccel object, not us). That handler is what tells kglobalacceld to
// stop grabbing (setInactive) and what drops the action from the client's
// action tables; without it the grab outlives the action and the client keeps
// a dangling QAction* that a later key press, rebind or service restart
// dereferences.

class KGlobalAccelBackend::Impl
{
public:
    struct Entry
    {
        QAction* action = nullptr;
        // Last values forwarded to KGlobalAccel. Kept so registerShortcut can
        // skip redundant setDefaultShortcut / setShortcut calls when neither
        // sequence has changed — each one costs a D-Bus round-trip and a
        // write into kglobalshortcutsrc. nullopt means "nothing sent yet",
        // which an empty sequence cannot stand in for: an empty default is a
        // real value that has to reach the service once.
        std::optional<QKeySequence> lastDefault;
        std::optional<QKeySequence> lastCurrent;
        // False for transient/ad-hoc shortcuts (e.g. the cancel-overlay
        // Escape grab bound only while a drag or overlay is up). Transient
        // ids register with NoAutoloading so the requested chord always wins
        // over a stored record, and the destructor purges their
        // kglobalshortcutsrc record so it does not outlive the process.
        bool persistent = true;
    };
    QHash<QString, Entry> entries;
    // Last KGlobalAccel::shortcut() value reported via triggersChanged, per
    // id. Gates the globalShortcutChanged fan-out so repeats don't trigger
    // redundant consumer rebuilds.
    QHash<QString, QList<QKeySequence>> lastReportedTriggers;
    // True while this backend is inside one of its own KGlobalAccel setters.
    // KGlobalAccel emits globalShortcutChanged INLINE from setShortcut when
    // the service hands back keys other than the ones sent (autoloading a
    // stored record), before the caller can seed lastReportedTriggers. That
    // echo is our own change, not an out-of-band one, so the handler drops
    // it and the caller seeds the gate right after.
    bool applying = false;
};

namespace {

/// Raises Impl::applying for the lifetime of the guard.
class ApplyingGuard
{
public:
    explicit ApplyingGuard(bool& flag)
        : m_flag(flag)
        , m_previous(flag)
    {
        m_flag = true;
    }
    ~ApplyingGuard()
    {
        m_flag = m_previous;
    }
    ApplyingGuard(const ApplyingGuard&) = delete;
    ApplyingGuard& operator=(const ApplyingGuard&) = delete;

private:
    bool& m_flag;
    bool m_previous;
};

} // namespace

KGlobalAccelBackend::KGlobalAccelBackend(QObject* parent)
    : IBackend(parent)
    , m_impl(std::make_unique<Impl>())
{
    // KGlobalAccel groups shortcuts by QCoreApplication::applicationName() at
    // the time setShortcut() is first called. An empty applicationName silently
    // groups every shortcut under "" — they show up under no component in
    // System Settings and persistent kglobalshortcutsrc entries are unfindable.
    // Assert in debug; warn in release. Callers MUST set applicationName before
    // constructing this backend (typically in main() before the daemon ctor).
    Q_ASSERT_X(!QCoreApplication::applicationName().isEmpty(), "KGlobalAccelBackend",
               "QCoreApplication::applicationName() must be set before constructing the backend");
    if (QCoreApplication::applicationName().isEmpty()) {
        qCWarning(lcPhosphorShortcuts)
            << "KGlobalAccelBackend: applicationName is empty — shortcuts will register under no component"
            << "and System Settings will not surface them. Set QCoreApplication::setApplicationName() first.";
    }
    qCInfo(lcPhosphorShortcuts) << "KGlobalAccelBackend: active (component:" << QCoreApplication::applicationName()
                                << ")";

    // Surface external rebinds (the user editing our component in System
    // Settings) as triggersChanged so consumers displaying bindings can
    // re-query currentTriggers(). The signal passes the QAction; it is ours
    // iff its objectName matches a registered id (registerShortcut()
    // objectNames every action to its id). Echoes of our own setters are
    // dropped (Impl::applying) and the rest are deduped against the last
    // reported value, because consumers rebuild their whole display model per
    // emission.
    connect(KGlobalAccel::self(), &KGlobalAccel::globalShortcutChanged, this,
            [this](QAction* action, const QKeySequence& /*seq*/) {
                if (!action || m_impl->applying) {
                    return;
                }
                const QString id = action->objectName();
                const auto it = m_impl->entries.constFind(id);
                if (it == m_impl->entries.constEnd() || it->action != action) {
                    return;
                }
                const QList<QKeySequence> seqs = KGlobalAccel::self()->shortcut(action);
                const auto lastIt = m_impl->lastReportedTriggers.constFind(id);
                if (lastIt != m_impl->lastReportedTriggers.constEnd() && *lastIt == seqs) {
                    return;
                }
                m_impl->lastReportedTriggers.insert(id, seqs);
                Q_EMIT triggersChanged(id);
            });
}

KGlobalAccelBackend::~KGlobalAccelBackend()
{
    // IMPORTANT: do NOT call KGlobalAccel::removeAllShortcuts on PERSISTENT
    // shortcuts here.
    //
    // removeAllShortcuts purges the binding from the persistent on-disk
    // registry (kglobalshortcutsrc). On a normal daemon shutdown that would
    // wipe every user-customised shortcut — users would reset to defaults
    // on each restart. Deleting the QAction is the right teardown for a
    // persistent id: the KGlobalAccel client's destroyed() handler sends
    // setInactive, which releases the grab and keeps the record.
    //
    // NON-PERSISTENT (transient) shortcuts are the opposite: they belong to
    // a UI state (drag in progress, overlay visible, layout picker open) and
    // carry no customisation, so their record is purged here on every
    // teardown. That includes a clean shutdown: a consumer stopping while one
    // of those states is up tears down the backend without unregistering the
    // transient id first. A crash skips this destructor entirely, which is
    // what IBackend::forgetShortcut is for.
    //
    // The QActions are parented to `this`, so Qt's ~QObject cleanup would
    // destroy them anyway. We delete explicitly to clear the hash table's
    // pointers in the right order (action gone, entry cleared) and avoid
    // relying on implicit child-destruction ordering.
    //
    // action->disconnect(this) before delete, never a bare disconnect(): see
    // the note above Impl. It severs only the triggered lambda, which
    // captures `this`, so a queued activation dispatched mid-teardown cannot
    // emit activated() on a half-destroyed backend, and it leaves the
    // client's destroyed() handler to release the grab.
    //
    // The ctor's globalShortcutChanged lambda derefs m_impl, which this
    // destructor destroys before ~QObject would sever the connection, so cut
    // it up front rather than leave a window where a nested dispatch touches
    // a dead Impl.
    disconnect(KGlobalAccel::self(), nullptr, this, nullptr);
    for (auto& entry : m_impl->entries) {
        if (entry.action) {
            entry.action->disconnect(this);
            if (!entry.persistent) {
                KGlobalAccel::self()->removeAllShortcuts(entry.action);
            }
            delete entry.action;
            entry.action = nullptr;
        }
    }
}

void KGlobalAccelBackend::registerShortcut(const QString& id, const QKeySequence& defaultSeq,
                                           const QKeySequence& currentSeq, const QString& description, bool persistent)
{
    auto& entry = m_impl->entries[id];
    const bool firstRegistration = !entry.action;
    if (firstRegistration) {
        entry.action = new QAction(description, this);
        entry.action->setObjectName(id);
        entry.action->setProperty("componentName", ownComponent());
        const QString idCopy = id;
        connect(entry.action, &QAction::triggered, this, [this, idCopy] {
            Q_EMIT activated(idCopy);
        });
    } else if (!description.isEmpty() && entry.action->text() != description) {
        entry.action->setText(description);
    }
    // Latest persistent flag wins. A re-bind that escalates a transient
    // grab to persistent (or vice versa) updates the destructor-time
    // purge decision accordingly. Same-id rebinds with a different flag
    // are uncommon but not forbidden by the IBackend contract.
    entry.persistent = persistent;

    ApplyingGuard applying(m_impl->applying);
    // setDefaultShortcut establishes the "reset to default" binding shown in
    // System Settings. Must be called with the compiled-in default (NOT the
    // user's current value) so "Reset to default" actually resets to the
    // factory default. Skip the call when the compiled-in default hasn't
    // changed since the last send — each call otherwise costs a D-Bus
    // round-trip and a kglobalshortcutsrc write.
    if (entry.lastDefault != defaultSeq) {
        KGlobalAccel::self()->setDefaultShortcut(entry.action, {defaultSeq});
        entry.lastDefault = defaultSeq;
    }
    // setShortcut actually grabs the key. Same "only if changed" gate.
    //
    // Persistent ids use the default autoloading flag, so a user override
    // stored in kglobalshortcutsrc (a System Settings rebind) wins over the
    // currentSeq passed here. Transient ids use NoAutoloading: they carry no
    // customisation, so a stored record can only be a stale one, and
    // autoloading it would make a re-register with a new chord keep the old
    // one.
    if (entry.lastCurrent != currentSeq) {
        if (persistent) {
            KGlobalAccel::self()->setShortcut(entry.action, {currentSeq});
        } else {
            KGlobalAccel::self()->setShortcut(entry.action, {currentSeq}, KGlobalAccel::NoAutoloading);
        }
        entry.lastCurrent = currentSeq;
    }
    // Seed the dedup gate with the post-set effective value, so a later
    // repeat of it is not reported as a change. Read back rather than assume
    // currentSeq: with autoloading a stored user override wins over what we
    // just pushed. KGlobalAccel::shortcut() reads the client's local table,
    // so this costs no round trip. Known boundary: if an external rebind has
    // landed in kglobalaccel's table but its change echo is still queued when
    // a re-register interleaves, this seed absorbs that echo and one display
    // refresh is lost. The grab itself stays correct, so the cost is a stale
    // label until the next report.
    m_impl->lastReportedTriggers.insert(id, KGlobalAccel::self()->shortcut(entry.action));
}

void KGlobalAccelBackend::updateShortcut(const QString& id, const QKeySequence& /*defaultSeq*/,
                                         const QKeySequence& newTrigger)
{
    // defaultSeq is intentionally unused: setDefaultShortcut is refreshed via
    // registerShortcut whenever the compiled-in default changes (Registry
    // re-invokes registerShortcut for default deltas; updateShortcut only
    // fires for currentSeq-only deltas). Avoiding a redundant
    // setDefaultShortcut call here also saves a D-Bus round-trip and a
    // kglobalshortcutsrc write per update.
    auto it = m_impl->entries.find(id);
    if (it == m_impl->entries.end() || !it->action) {
        qCWarning(lcPhosphorShortcuts) << "KGlobalAccel updateShortcut: unknown id" << id;
        return;
    }
    if (it->lastCurrent == newTrigger) {
        return;
    }
    // NoAutoloading, unlike a persistent registerShortcut: registration
    // already SAVED a binding to kglobalshortcutsrc, so with the default
    // autoloading flag this call would load that saved value back and
    // silently ignore newTrigger — making every programmatic rebind a no-op
    // (verified against a live daemon by test_kglobalaccel_backend).
    // updateShortcut only fires when the consumer's own config value actually
    // changed (Registry short-circuits same-sequence rebinds), i.e. an
    // explicit rebind that must win; NoAutoloading applies it AND persists
    // it, so System Settings stays in sync. Startup adoption of a System
    // Settings override still works — that path is registerShortcut's
    // autoloading setShortcut, not this one.
    {
        ApplyingGuard applying(m_impl->applying);
        KGlobalAccel::self()->setShortcut(it->action, {newTrigger}, KGlobalAccel::NoAutoloading);
    }
    it->lastCurrent = newTrigger;
    // Same self-echo seeding as registerShortcut — see the note there.
    m_impl->lastReportedTriggers.insert(id, KGlobalAccel::self()->shortcut(it->action));
}

void KGlobalAccelBackend::unregisterShortcut(const QString& id)
{
    auto it = m_impl->entries.find(id);
    if (it == m_impl->entries.end()) {
        return;
    }
    QAction* action = it->action;
    m_impl->entries.erase(it);
    // Drop the dedup seed with the entry: a stale value would suppress the
    // first legitimate triggersChanged after a later re-register of the
    // same id (the transient drag-cancel Escape registers per drag), and
    // ids never re-registered would accumulate dead map entries.
    m_impl->lastReportedTriggers.remove(id);
    if (action) {
        // Explicit unregister IS the one path where we want to clear the
        // persistent binding — the consumer has asked to drop the shortcut
        // entirely (e.g. WindowDragAdaptor releasing the dynamic Escape
        // shortcut after a drag ends). Contrast with the destructor, which
        // preserves the on-disk state.
        //
        // Our triggered lambda is cut first so an activation queued before
        // the deferred delete does not report an id that is gone. The
        // client's own tables were already cleared by removeAllShortcuts.
        action->disconnect(this);
        KGlobalAccel::self()->removeAllShortcuts(action);
        action->deleteLater();
    }
}

void KGlobalAccelBackend::suspendShortcut(const QString& id)
{
    auto it = m_impl->entries.find(id);
    if (it == m_impl->entries.end()) {
        return;
    }
    if (!it->persistent) {
        // A transient grab carries no user customisation, and its on-disk
        // record is the very thing the destructor purges, so parking one is
        // the same operation as dropping it.
        unregisterShortcut(id);
        return;
    }
    QAction* action = it->action;
    m_impl->entries.erase(it);
    // Drop the dedup seed with the entry, exactly as unregisterShortcut does:
    // a stale value would swallow the first legitimate triggersChanged after
    // the id is resumed.
    m_impl->lastReportedTriggers.remove(id);
    if (action) {
        // Destroying the QAction is what releases the grab: the KGlobalAccel
        // client's destroyed() handler sends setInactive, which makes
        // kglobalacceld stop grabbing the keys while it keeps the
        // kglobalshortcutsrc record, and drops the action from the client's
        // tables. That is the whole point here. A later registerShortcut()
        // builds a fresh action under the same objectName and its autoloading
        // setShortcut picks the stored value back up.
        //
        // disconnect(this), never a bare disconnect(): the bare form also cut
        // that destroyed() handler, so no setInactive was sent, the keys
        // stayed grabbed, and the client kept the freed QAction* in its tables
        // for a revived chord to find on its next press. Cutting our own
        // triggered lambda still matters, for the destructor's reason: it
        // captures `this`, and a queued activation dispatched mid-delete would
        // emit activated() for an id that is no longer registered.
        //
        // Deleted synchronously rather than deleteLater'd: a resume in the same
        // event-loop turn would otherwise create the replacement action while
        // the old one is still pending destruction, and the deferred
        // destroyed() handler would then mark the just re-established id
        // inactive.
        action->disconnect(this);
        delete action;
    }
}

void KGlobalAccelBackend::storeShortcut(const QString& id, const QKeySequence& seq)
{
    if (m_impl->entries.contains(id)) {
        // Held: the live registration is the record's writer.
        if (seq.isEmpty()) {
            unregisterShortcut(id);
        } else {
            updateShortcut(id, QKeySequence(), seq);
        }
        return;
    }
    if (seq.isEmpty()) {
        forgetShortcut(id);
        return;
    }
    // setForeignShortcutKeys on our own component sets the record's keys with
    // NoAutoloading and without SetPresent, so kglobalacceld stores the chord
    // without grabbing it, and the next autoloading registration hands it
    // back. An id the service has no record of is ignored there, and the next
    // registration then creates the record from its own arguments.
    setForeignShortcuts(ownComponent(), id, {seq});
}

void KGlobalAccelBackend::forgetShortcut(const QString& id)
{
    if (m_impl->entries.contains(id)) {
        unregisterShortcut(id);
        return;
    }
    // The same call KGlobalAccel's removeAllShortcuts sends, spoken raw
    // because the client API needs a live QAction for it. kglobalacceld
    // deletes the record and with it any grab the record still holds.
    // Asynchronous like the client's own: nothing waits on the answer, and
    // the session-bus connection keeps it ordered ahead of any later
    // registration of the same id.
    QDBusMessage call = kglobalaccelCall(QStringLiteral("unregister"));
    call << ownComponent() << id;
    QDBusConnection::sessionBus().asyncCall(call);
}

std::optional<QStringList> KGlobalAccelBackend::storedShortcutIds() const
{
    // allActionsForComponent takes an action id of which only the component
    // field is read, and answers aas: one action id per stored record.
    QDBusMessage call = kglobalaccelCall(QStringLiteral("allActionsForComponent"));
    call << QStringList{ownComponent(), QString(), QString(), QString()};
    const QDBusMessage reply = QDBusConnection::sessionBus().call(call, QDBus::Block, kRawCallTimeoutMs);
    if (reply.type() != QDBusMessage::ReplyMessage || reply.arguments().isEmpty()) {
        qCWarning(lcPhosphorShortcuts) << "allActionsForComponent query failed — reply type" << reply.type() << ":"
                                       << reply.errorMessage();
        return std::nullopt;
    }
    const QVariant payload = reply.arguments().constFirst();
    if (!payload.canConvert<QDBusArgument>()) {
        qCWarning(lcPhosphorShortcuts) << "allActionsForComponent returned an unexpected payload";
        return std::nullopt;
    }
    const QDBusArgument arg = payload.value<QDBusArgument>();
    if (arg.currentSignature() != QLatin1String("aas")) {
        qCWarning(lcPhosphorShortcuts) << "allActionsForComponent returned signature" << arg.currentSignature()
                                       << "instead of aas";
        return std::nullopt;
    }
    // ActionUnique is the second field of a KGlobalAccel action id.
    constexpr int actionUniqueField = 1;
    QStringList ids;
    arg.beginArray();
    while (!arg.atEnd()) {
        QStringList actionId;
        arg >> actionId;
        const QString unique = actionId.value(actionUniqueField);
        if (!unique.isEmpty()) {
            ids.append(unique);
        }
    }
    arg.endArray();
    return ids;
}

std::optional<QStringList> KGlobalAccelBackend::currentTriggers(const QString& id) const
{
    const auto it = m_impl->entries.constFind(id);
    if (it == m_impl->entries.constEnd() || !it->action) {
        // Unknown id — nothing to report; the caller's own value stands.
        return std::nullopt;
    }
    // KGlobalAccel::shortcut() answers from the client's local table, which
    // holds what the service handed back at registration (including a stored
    // user override) and follows external rebinds as their change signals
    // arrive. No round trip, so consumers like the shortcut cheatsheet can
    // query every id in one burst. An engaged-but-empty result is
    // AUTHORITATIVE: the user cleared the binding, and falling back to the
    // stored sequence would display a key that no longer works.
    QStringList out;
    for (const QKeySequence& seq : KGlobalAccel::self()->shortcut(it->action)) {
        if (!seq.isEmpty()) {
            out.append(seq.toString(QKeySequence::PortableText));
        }
    }
    return out;
}

void KGlobalAccelBackend::flush()
{
    // KGlobalAccel operations are synchronous per-call; there's no batch to
    // commit. Emit ready so Registry consumers can treat the two backends
    // uniformly.
    Q_EMIT ready();
}

bool KGlobalAccelBackend::setForeignShortcuts(const QString& componentName, const QString& actionName,
                                              const QList<QKeySequence>& sequences)
{
    // kglobalaccel's setForeignShortcutKeys(actionId as, keys a(ai)) — the
    // KCM's rebind API, spoken raw because the KGlobalAccel client library
    // only exposes it for the caller's own actions. Each a(ai) element is
    // one SEQUENCE (a struct wrapping its up-to-four chord ints); passing
    // several elements sets every key the action answers to, so a restore
    // reinstates the action's full binding. The legacy setForeignShortcut(ai)
    // variant is deliberately NOT used: it reads each int as a separate
    // alternate, which turned a user's multi-chord sequence into bogus
    // alternates.
    //
    // BLOCKING, up to kRawCallTimeoutMs per action, and a caller that walks a
    // list of actions pays it per action when kglobalacceld is wedged.
    // Accepted rather than made async: the realistic latency is a
    // sub-millisecond local round trip, and a MISSING service fails at once
    // rather than timing out, because kglobalaccelCall turns autostart off.
    // That covers the common slow case, a teardown at logout after the
    // compositor hosting kglobalacceld has gone.
    const QStringList actionId{componentName, actionName, QString(), QString()};
    // Registered once per process, not once per call: qDBusRegisterMetaType is
    // idempotent but takes a lock and hashes the type on every invocation, and
    // this runs per action during a walk.
    static const bool wireTypesRegistered = [] {
        qDBusRegisterMetaType<detail::DBusKeySequence>();
        qDBusRegisterMetaType<QList<detail::DBusKeySequence>>();
        return true;
    }();
    Q_UNUSED(wireTypesRegistered);
    QList<detail::DBusKeySequence> wire;
    if (sequences.isEmpty()) {
        // Explicit "unbound": one sequence of a single 0 key.
        detail::DBusKeySequence unbound;
        unbound.keys.append(0);
        wire.append(unbound);
    } else {
        for (const QKeySequence& sequence : sequences) {
            detail::DBusKeySequence seq;
            for (int i = 0; i < sequence.count(); ++i) {
                seq.keys.append(sequence[i].toCombined());
            }
            if (seq.keys.isEmpty()) {
                seq.keys.append(0);
            }
            wire.append(seq);
        }
    }
    QDBusMessage call = kglobalaccelCall(QStringLiteral("setForeignShortcutKeys"));
    call << actionId << QVariant::fromValue(wire);
    const QDBusMessage reply = QDBusConnection::sessionBus().call(call, QDBus::Block, kRawCallTimeoutMs);
    if (reply.type() == QDBusMessage::ErrorMessage) {
        qCWarning(lcPhosphorShortcuts) << "setForeignShortcutKeys failed for" << componentName << actionName << ":"
                                       << reply.errorMessage();
        return false;
    }
    return true;
}

std::optional<QList<QKeySequence>> KGlobalAccelBackend::foreignShortcuts(const QString& componentName,
                                                                         const QString& actionName) const
{
    const QStringList actionId{componentName, actionName, QString(), QString()};
    QDBusMessage call = kglobalaccelCall(QStringLiteral("shortcutKeys"));
    call << actionId;
    const QDBusMessage reply = QDBusConnection::sessionBus().call(call, QDBus::Block, kRawCallTimeoutMs);
    if (reply.type() != QDBusMessage::ReplyMessage || reply.arguments().isEmpty()) {
        // The QUERY failed (service down, timed out, unexpected reply shape) —
        // report "cannot answer", never an empty binding. An empty list here
        // told the takeover caller the action was already unbound, so it wrote
        // no backup and a later restore put the unbound sentinel on top of a
        // chord the user still had.
        // reply.type() as well as the message: this branch also catches a
        // well-formed ReplyMessage that carried no arguments, and an
        // errorMessage() on one of those is empty, which logged a failure with
        // no stated reason.
        qCWarning(lcPhosphorShortcuts) << "shortcutKeys query failed for" << componentName << actionName
                                       << "— reply type" << reply.type() << ":" << reply.errorMessage();
        return std::nullopt;
    }
    const QVariant payload = reply.arguments().constFirst();
    if (!payload.canConvert<QDBusArgument>()) {
        qCWarning(lcPhosphorShortcuts) << "shortcutKeys returned an unexpected payload for" << componentName
                                       << actionName;
        return std::nullopt;
    }
    const QDBusArgument arg = payload.value<QDBusArgument>();
    // Checked before walking: the loop below opens an array of structures of
    // int arrays, and a reply of any other shape would be read as garbage or
    // leave the argument mid-container.
    if (arg.currentSignature() != QLatin1String("a(ai)")) {
        qCWarning(lcPhosphorShortcuts) << "shortcutKeys returned signature" << arg.currentSignature() << "for"
                                       << componentName << actionName << "instead of a(ai)";
        return std::nullopt;
    }
    // Reply is a(ai): a list of sequences, each a struct holding key ints.
    // ALL entries are read — every key the action answers to is part of its
    // binding and must survive a backup/restore round trip.
    QList<QKeySequence> sequences;
    arg.beginArray();
    while (!arg.atEnd()) {
        QList<int> keys;
        arg.beginStructure();
        arg >> keys;
        arg.endStructure();
        if (!keys.isEmpty() && keys.first() != 0) {
            // Four chords, deliberately: QKeySequence itself holds at most
            // four, so a fifth int could not be represented however it were
            // read, and value() returns 0 for the short lists that are the
            // normal case. The lossiness is the type's, not an oversight here.
            sequences.append(QKeySequence(keys.value(0), keys.value(1), keys.value(2), keys.value(3)));
        }
    }
    arg.endArray();
    return sequences;
}

} // namespace PhosphorShortcuts
