// SPDX-FileCopyrightText: 2026 fuddlesworth
// SPDX-License-Identifier: LGPL-2.1-or-later

#pragma once

#include <QHash>
#include <QKeySequence>
#include <QObject>
#include <QPointer>
#include <QString>
#include <QStringList>
#include <QVector>

#include <functional>
#include <optional>

#include "phosphorshortcuts_export.h"

namespace PhosphorShortcuts {

class IBackend;

/**
 * Consumer-facing facade over IBackend.
 *
 * Owns a table of `(id, default sequence, current sequence, description,
 * optional callback)` rows. Forwards registration + rebind calls to the
 * backend and fans activation signals out as either per-id callbacks or the
 * triggered() signal — consumers may use either pattern interchangeably.
 *
 * The Registry does not own the backend; the caller keeps the backend alive
 * for the Registry's lifetime. Passing a null backend is a programming error.
 */
class PHOSPHORSHORTCUTS_EXPORT Registry : public QObject
{
    Q_OBJECT
public:
    struct Binding
    {
        QString id;
        QKeySequence defaultSeq;
        QKeySequence currentSeq;
        QString description;
    };

    explicit Registry(IBackend* backend, QObject* parent = nullptr);
    ~Registry() override;

    /**
     * Register a shortcut. Safe to call multiple times for the same id —
     * subsequent calls update the default sequence, description, and callback
     * in place but PRESERVE the current sequence (any user-applied rebind is
     * kept). Takes effect after flush().
     *
     * @param callback Optional. Invoked on activation in addition to the
     *                 triggered() signal. Nullptr callbacks are stored but
     *                 never invoked; consumers relying purely on the signal
     *                 can omit the argument.
     * @param persistent If false, the binding is considered transient (e.g. a
     *                   grab bound around a specific UI state). It is excluded
     *                   from bindings(true) enumeration and forwarded to
     *                   IBackend::registerShortcut, where it decides whether
     *                   the requested sequence overrides the platform's stored
     *                   record and whether that record is purged on teardown.
     *                   A flip of the flag on a later bind() re-registers.
     *                   Defaults to true.
     *
     * A fresh entry starts active (see setActive()); a later bind() keeps the
     * entry's active flag.
     *
     * Note: a description reaches the backend only with a registerShortcut
     * call, never with an update (IBackend::updateShortcut carries no
     * description). A description changed by a later bind() therefore stays
     * local until something makes flush() register again: a changed default,
     * a flipped persistent flag, or the entry coming back after setActive()
     * parked it or rebind() cleared it.
     */
    void bind(const QString& id, const QKeySequence& defaultSeq, const QString& description = {},
              std::function<void()> callback = {}, bool persistent = true);

    /**
     * Change the active binding for an already-registered id. Takes effect
     * after flush(). Unknown ids are logged and ignored. Passing an empty
     * QKeySequence releases the backend grab immediately (never leaves an
     * empty sequence registered) but KEEPS the entry, so a later rebind()
     * to a non-empty sequence re-registers it without a fresh bind().
     */
    void rebind(const QString& id, const QKeySequence& seq);

    /**
     * Rebind / read another component's global shortcut (IBackend foreign
     * pass-throughs; only the KGlobalAccel backend supports them).
     *
     * foreignShortcuts is tri-state — see IBackend::foreignShortcuts. A
     * disengaged optional means "could not read", and a caller about to steal
     * the chord must treat that as a hard stop rather than as "unbound":
     * clearing without a backup strands the user's binding.
     */
    bool setForeignShortcuts(const QString& componentName, const QString& actionName,
                             const QList<QKeySequence>& sequences);
    std::optional<QList<QKeySequence>> foreignShortcuts(const QString& componentName, const QString& actionName) const;

    /**
     * Drop a binding entirely. Releases any key grab and forgets the
     * callback. Idempotent. Applied immediately — NOT batched until flush()
     * — because the backends' unregister paths all act synchronously or
     * with trivial state, and a late flush would be surprising for a
     * "release this grab now" API.
     */
    void unbind(const QString& id);

    /**
     * Like unbind() for an id this registry holds. For an id it does not
     * hold, asks the backend to delete whatever persistent record the
     * platform still keeps for it (IBackend::forgetShortcut), which is how a
     * consumer clears a transient grab an earlier, crashed run left behind.
     * Applied immediately. Only for transient ids: on a persistent id this
     * destroys the user's customisation.
     */
    void purge(const QString& id);

    /// Every id the platform keeps a record for under this process's
    /// component (IBackend::storedShortcutIds). nullopt when the backend
    /// keeps none or the query failed.
    std::optional<QStringList> storedShortcutIds() const;

    /**
     * Park or revive a binding without forgetting it and without destroying
     * what the platform persists about it. Takes effect after flush().
     * Unknown ids are logged and ignored. Entries are active when bound.
     *
     * This is how a consumer gates a whole family of shortcuts on a feature
     * switch. Rebinding the family to an empty sequence looks equivalent and
     * is not: that routes through IBackend::unregisterShortcut, which on
     * KGlobalAccel wipes the user's saved chords for every id in the family,
     * so turning the feature back on hands back the compiled-in defaults
     * rather than what the user had. An inactive entry goes through
     * IBackend::suspendShortcut instead, and reviving it re-registers with
     * the stored binding intact.
     *
     * An inactive entry reports no effectiveTriggers(): nothing the user can
     * press reaches it. shortcut() still reports the stored sequence, which
     * is what the consumer would grab again on revival.
     *
     * A rebind() aimed at a parked entry is not lost. flush() writes it into
     * the platform's record (IBackend::storeShortcut) so the revival grabs
     * the new chord, including across a restart while still parked. The same
     * holds for a rebind() that lands in the same batch as the park, and a
     * rebind() to an empty sequence deletes the record. The write-through
     * only follows this registry's own rebinds after the entry's first
     * flush: the sequences applied before the first flush are the consumer's
     * stored config, and writing those would overwrite a chord the user set
     * in the platform's own settings while the entry was parked.
     *
     * A fresh entry is active. bind() on an existing entry keeps its flag.
     */
    void setActive(const QString& id, bool active);

    /// Whether the id is currently active. Unknown ids report false.
    bool isActive(const QString& id) const;

    /**
     * Forward queued bind/rebind ops to the backend. Does NOT include
     * unbind() — those are applied immediately at the call site. Matches
     * the backend's queue-then-flush model for register / update.
     *
     * ready() fires after flushes settle, including no-op flushes where no
     * entry actually changed, so consumers can gate UI on "any flush has
     * settled" without per-entry bookkeeping. Do NOT assume a 1:1
     * flush→ready count: during asynchronous backend bring-up
     * (PortalBackend before its session handle lands) multiple queued
     * flushes coalesce into a single ready(). If you need "only fire when
     * something actually changed", track that on the caller side.
     */
    void flush();

    QKeySequence shortcut(const QString& id) const;

    /**
     * Display strings for the key(s) the user EFFECTIVELY has to press for
     * an id right now. Prefers the backend's read-back
     * (IBackend::currentTriggers — which sees out-of-process overrides like
     * System Settings rebinds, INCLUDING a cleared binding, which reports
     * engaged-empty and is honored as unbound); falls back to the
     * registry's own current sequence only when the backend cannot report
     * at all. Empty means the id is genuinely unbound (or unknown).
     *
     * The strings are DISPLAY-ONLY and not format-stable across backends
     * (see IBackend::currentTriggers): KGlobalAccel yields PortableText
     * while Portal relays the compositor's localized description. Don't
     * string-compare results across backends or parse them back into
     * QKeySequence.
     *
     * A consumer that nonetheless parses these back (to normalize or
     * token-compare them) must degrade gracefully when a string does not
     * parse, and a change to this method's output format must keep such
     * consumers in mind.
     */
    QStringList effectiveTriggers(const QString& id) const;

    /**
     * Enumerate registered bindings, sorted by id for deterministic output.
     *
     * @param persistentOnly If true, transient bindings (those registered with
     *                       persistent=false) are excluded. Intended for
     *                       settings UIs that should not expose internal
     *                       ad-hoc grabs to the user. Defaults to false so
     *                       tests and library-internal callers see everything.
     */
    QVector<Binding> bindings(bool persistentOnly = false) const;

Q_SIGNALS:
    /**
     * Emitted on every activation, regardless of whether the binding has a
     * callback. Use this for centralised dispatch (one slot, switch on id).
     *
     * Signature matches IBackend::activated (QString by value) so the two
     * signals can be cross-connected without adapter slots.
     */
    void triggered(QString id);

    /**
     * Forwarded from IBackend::ready().
     */
    void ready();

    /**
     * Forwarded from IBackend::triggersChanged for ids this registry owns —
     * the effective binding changed outside the bind/rebind flow (external
     * rebind, compositor assignment). Re-query effectiveTriggers() on it.
     *
     * Always delivered from the event loop, never from inside a call into
     * this registry: a backend may report while flush() walks the entries,
     * and a slot that binds or unbinds in response would otherwise mutate
     * the table under the walk.
     */
    void triggersChanged(QString id);

private Q_SLOTS:
    void onBackendActivated(QString id);
    void onBackendReady();

private:
    struct Entry
    {
        Binding binding;
        std::function<void()> callback;
        // Last values successfully pushed to the backend. Used to decide
        // whether flush() should re-register (default changed) or update-only
        // (current changed), and to short-circuit no-op flushes.
        QKeySequence lastSentDefault;
        QKeySequence lastSentCurrent;
        bool registered = false; // has registerShortcut been sent yet?
        // Persistent flag last forwarded to the backend. flush() re-registers
        // when this drifts from `persistent` so a re-bind() that flips the flag
        // without changing the key still reaches the backend — the backend's
        // crash-purge decision (KGlobalAccelBackend) depends on the latest
        // value. Only meaningful once `registered` is true.
        bool lastSentPersistent = true;
        bool persistent = true; // surfaces in bindings(persistentOnly=true)
        // Feature gate, see setActive(). An inactive entry keeps its sequence
        // and its callback but holds no grab; flush() parks it through
        // IBackend::suspendShortcut instead of unregistering it, so the
        // platform's record of the user's own chord survives the gate.
        bool active = true;
        // Has flush() visited this entry at least once? Sequences applied
        // before that are the consumer's stored config, not a user change,
        // so they never arm storePending (see setActive()).
        bool settled = false;
        // currentSeq moved while the backend held no registration for the
        // id. The platform's record still carries the old chord, so flush()
        // writes the new one through IBackend::storeShortcut before the id is
        // registered again (or while it stays parked).
        bool storePending = false;
    };

    QPointer<IBackend> m_backend;
    QHash<QString, Entry> m_entries;
};

} // namespace PhosphorShortcuts
