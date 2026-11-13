// SPDX-FileCopyrightText: 2026 fuddlesworth
// SPDX-License-Identifier: LGPL-2.1-or-later

#pragma once

#include <QKeySequence>
#include <QObject>
#include <QString>
#include <QStringList>

#include <optional>

#include "phosphorshortcuts_export.h"

namespace PhosphorShortcuts {

/**
 * Pluggable global shortcut backend.
 *
 * Implementations bridge to a specific binding mechanism: KGlobalAccel
 * (KDE), XDG Desktop Portal GlobalShortcuts, D-Bus trigger fallback, or a
 * future compositor-native grabber.
 *
 * Shortcuts are addressed by stable string id. The library does not expose
 * QAction in this interface on purpose — QAction is kept as an implementation
 * detail of the KGlobalAccel backend, since that API requires it.
 *
 * Concurrency: all methods and signals run on the thread that owns the
 * backend (typically the GUI thread). Not thread-safe.
 */
class PHOSPHORSHORTCUTS_EXPORT IBackend : public QObject
{
    Q_OBJECT
public:
    explicit IBackend(QObject* parent = nullptr)
        : QObject(parent)
    {
    }
    ~IBackend() override = default;

    /**
     * Register a new shortcut id.
     *
     * @param id          Stable string id. The library imposes no prefix
     *                    convention — Phosphor uses plain snake_case ids
     *                    like "move_window_left" because KGlobalAccel and
     *                    XDG Portal persist the id verbatim; renaming is an
     *                    on-disk rename users pay for. Pick a scheme that
     *                    won't need to churn.
     * @param defaultSeq  Compiled-in default key sequence — the "factory"
     *                    value a user can reset to. KGlobalAccel records
     *                    this via setDefaultShortcut so System Settings'
     *                    "Reset to default" resets to the correct value.
     *                    Portal backends use this as `preferred_trigger`
     *                    (advisory — the compositor assigns the actual key).
     *                    DBusTrigger ignores it entirely.
     * @param currentSeq  The key sequence to actually grab now. Usually the
     *                    user's customised value read from config; equals
     *                    defaultSeq on a fresh install. May be empty (no grab).
     * @param description Human-readable label surfaced in portal settings
     *                    UIs and kglobalaccel listings.
     * @param persistent  When false, the binding is transient: it carries no
     *                    user customisation and exists only while some UI
     *                    state is up. Two duties follow for a backend that
     *                    keeps a record of its own (KGlobalAccel's
     *                    kglobalshortcutsrc). The requested currentSeq must
     *                    win over whatever that record holds, so a re-register
     *                    with a new chord really moves the grab. And the
     *                    record must not outlive the process that made it:
     *                    the backend purges it on destruction. A crash skips
     *                    that purge, and kglobalacceld has no handling for a
     *                    client that vanishes while its grab is present, so
     *                    the key stays grabbed for the rest of the session.
     *                    forgetShortcut() is the consumer's tool for clearing
     *                    such leftovers at its next start (discussion #461
     *                    item 14). Persistent (true) is the historical
     *                    default and the only correct value for
     *                    user-customizable shortcuts.
     *
     * A backend may apply this at once (KGlobalAccel does) or hold it until
     * flush() (Portal batches into one BindShortcuts). Call flush() either way.
     */
    virtual void registerShortcut(const QString& id, const QKeySequence& defaultSeq, const QKeySequence& currentSeq,
                                  const QString& description, bool persistent = true) = 0;

    /**
     * Change the active binding for an already-registered id. Takes both
     * sequences — defaultSeq stays for backends that need to keep the
     * "factory default" target current (PortalBackend's preferred_trigger,
     * which is keyed off defaultSeq for consistency with registerShortcut),
     * currentSeq is the new value to grab.
     *
     * KGlobalAccel backend ignores defaultSeq here because the default
     * target is independently refreshed via registerShortcut whenever the
     * compiled-in default changes (Registry re-invokes registerShortcut for
     * defaultSeq changes; updateShortcut only fires on currentSeq-only
     * deltas).
     *
     * Does NOT carry a description — description updates require a fresh
     * registerShortcut call. Like registerShortcut, a backend may apply it at
     * once or at the next flush(); call flush() either way.
     */
    virtual void updateShortcut(const QString& id, const QKeySequence& defaultSeq, const QKeySequence& newTrigger) = 0;

    /**
     * Release the key grab for an id. Idempotent; unknown ids are ignored.
     * This call is NOT queued — backends apply it immediately (subject to
     * per-backend semantics; see PortalBackend notes in the .cpp).
     *
     * PortalBackend caveat: XDG GlobalShortcuts has no per-id release, so
     * this is a LOCAL-ONLY clear on that backend (onActivated will drop
     * the event, but the key stays grabbed compositor-side until the
     * session closes). Consumers needing truly transient grabs on Portal
     * compositors should bind once and gate via a flag inside the
     * callback.
     */
    virtual void unregisterShortcut(const QString& id) = 0;

    /**
     * Release the key grab for an id WITHOUT destroying whatever the
     * platform persists about it. The id can be brought back later with a
     * plain registerShortcut(), which picks the platform's stored binding
     * back up. While the id is parked that stored binding no longer follows
     * updateShortcut() (there is no live registration to update), so a
     * consumer whose own sequence moves while parked keeps the record in step
     * with storeShortcut().
     *
     * This exists because unregisterShortcut() is deliberately destructive
     * on KGlobalAccel: it calls removeAllShortcuts, the one path that wipes
     * the record in kglobalshortcutsrc, so using it to park a feature-gated
     * family destroys every chord the user customised for that family. A
     * feature toggled off and on again must give the user back the chords
     * they had.
     *
     * The default implementation forwards to unregisterShortcut(), which is
     * the right answer for backends that keep no user-owned record of their
     * own (Portal grabs live for the session, the D-Bus fallback has no
     * grabs at all). KGlobalAccelBackend overrides it.
     */
    virtual void suspendShortcut(const QString& id)
    {
        unregisterShortcut(id);
    }

    /**
     * Write @p seq into the platform's persistent record for an id that this
     * backend does not currently hold (never registered, or parked through
     * suspendShortcut), so the next registerShortcut() picks it up. Never
     * grabs a key. An empty @p seq deletes the record, exactly as
     * forgetShortcut() does. A record the platform does not know is left
     * alone: the next registerShortcut() creates it from its own arguments.
     *
     * An id the backend does hold is routed through updateShortcut() (or
     * unregisterShortcut() for an empty @p seq) instead.
     *
     * The default does nothing, which is right for backends with no record
     * that outlives a registration (Portal, the D-Bus fallback).
     */
    virtual void storeShortcut(const QString& id, const QKeySequence& seq)
    {
        Q_UNUSED(id);
        Q_UNUSED(seq);
    }

    /**
     * Delete the platform's persistent record for an id, releasing any grab
     * that record still holds. Meant for ids this process does not hold, such
     * as a transient grab a crashed earlier run left behind (see the
     * persistent parameter of registerShortcut). An id the backend does hold
     * goes through unregisterShortcut(). Never call it on a persistent id the
     * user may have customised: it destroys the customisation.
     *
     * The default does nothing (no record to delete).
     */
    virtual void forgetShortcut(const QString& id)
    {
        Q_UNUSED(id);
    }

    /**
     * Every id the platform holds a persistent record for under this
     * process's component, registered by this run or not. nullopt when the
     * backend keeps no such record or the query failed. Pair with
     * forgetShortcut() to clear leftovers from an earlier run.
     */
    virtual std::optional<QStringList> storedShortcutIds() const
    {
        return std::nullopt;
    }

    /**
     * Whether suspendShortcut() and unregisterShortcut() actually release the
     * key grab. False on Portal, whose spec has no per-id release: a grab
     * stays with the session until it closes. A consumer that parks an id
     * right after registering it should not register it at all on such a
     * backend, or the park leaves a dead grab for the whole session.
     */
    virtual bool suspendReleasesGrab() const
    {
        return true;
    }

    /**
     * Whether an id with an empty current sequence must still be registered.
     * False for the key-grabbing backends, where registering nothing is the
     * stale-grab hazard of discussion #155, so Registry keeps such an id out
     * of the backend. True for the D-Bus trigger fallback, which grabs no
     * keys and can only fire ids it has been told about.
     */
    virtual bool registersUnboundIds() const
    {
        return false;
    }

    /**
     * Commit any queued register/update ops. Emits ready() once the
     * underlying backend has acknowledged the batch (may be synchronous or
     * asynchronous depending on backend). unregisterShortcut() is NOT
     * queued — see its doc above.
     */
    virtual void flush() = 0;

    /**
     * The key sequence(s) the backend believes are EFFECTIVELY bound for an
     * id right now — i.e. what the user actually has to press, including any
     * override applied outside this process (System Settings rebind on
     * KGlobalAccel, compositor-assigned trigger on Portal).
     *
     * Returned as display strings rather than QKeySequence because the XDG
     * Portal only reports a human-readable `trigger_description`; backends
     * that do have structured sequences (KGlobalAccel) return
     * QKeySequence::toString(QKeySequence::PortableText) per sequence. The
     * strings are DISPLAY-ONLY and not format-stable across backends —
     * Portal relays the compositor's localized description verbatim while
     * KGlobalAccel yields PortableText — so callers must not string-compare
     * results across backends or parse them back into sequences.
     *
     * Tri-state on purpose:
     *  - std::nullopt   → this backend cannot report for the id; callers
     *    fall back to their own stored current sequence.
     *  - engaged, empty → the backend reports the id as unbound as far as
     *    it can tell (e.g. the user cleared the key in System Settings).
     *    Callers must NOT fall back — the stored sequence is stale. This is
     *    a best-effort report, not a hard guarantee that the user cleared
     *    the key: a momentarily unavailable binding service (kglobalacceld
     *    down, grab not yet landed) can also read back empty, in which case
     *    the display shows unbound until the next triggersChanged report.
     *  - engaged, non-empty → the effective trigger strings.
     * Folding the first two into one empty list made a cleared binding
     * display as its stale stored value.
     *
     * The default implementation reports nullopt (correct for the D-Bus
     * trigger fallback, which has no key grabs at all).
     */
    virtual std::optional<QStringList> currentTriggers(const QString& id) const
    {
        Q_UNUSED(id);
        return std::nullopt;
    }

    /**
     * Rebind ANOTHER component's global shortcut (kglobalaccel's
     * setForeignShortcutKeys, the API the Shortcuts KCM uses). The list
     * carries the action's FULL binding, every sequence it should answer to,
     * so a restore puts back exactly what foreignShortcuts() reported; an
     * empty list clears the binding. Only the KGlobalAccel backend can do
     * this; the default refuses (false).
     *
     * True means only that the call reached the binding service without a
     * D-Bus error. The service answers an action it does not know with
     * silence rather than an error, so true does not prove the action exists.
     */
    virtual bool setForeignShortcuts(const QString& componentName, const QString& actionName,
                                     const QList<QKeySequence>& sequences)
    {
        Q_UNUSED(componentName);
        Q_UNUSED(actionName);
        Q_UNUSED(sequences);
        return false;
    }

    /**
     * The current bindings of another component's action, every sequence it
     * answers to, in the order the binding service reports them. kglobalacceld
     * keeps an action's keys as a set, so there is no primary and the order
     * carries no meaning.
     *
     * Tri-state, for the same reason currentTriggers() above is:
     *  - std::nullopt   → the query FAILED or this backend cannot answer (no
     *    foreign-rebind support, binding service unreachable, malformed
     *    reply). The caller must NOT proceed to clear the action: it has no
     *    backup, so a later restore would write the "unbound" sentinel over a
     *    binding the user still has.
     *  - engaged, empty → the service reports no keys. That is a genuinely
     *    unbound action, or an action the service does not know at all: it
     *    answers both the same way. Either way there is nothing to back up
     *    and nothing to steal.
     *  - engaged, non-empty → the action's full binding.
     * Folding the first two into one empty list made an unreachable
     * kglobalacceld look exactly like an already-unbound action.
     *
     * The default reports nullopt: a backend without setForeignShortcuts
     * cannot answer this either.
     */
    virtual std::optional<QList<QKeySequence>> foreignShortcuts(const QString& componentName,
                                                                const QString& actionName) const
    {
        Q_UNUSED(componentName);
        Q_UNUSED(actionName);
        return std::nullopt;
    }

Q_SIGNALS:
    /**
     * Emitted when the backend observes the user triggering a registered
     * shortcut. The id matches what was passed to registerShortcut().
     */
    void activated(QString id);

    /**
     * Emitted after flush() completes. Consumers that need to know when
     * initial registration is live (e.g. to un-grey a UI element) connect
     * here.
     */
    void ready();

    /**
     * Emitted when the effective binding for an id changes outside the
     * register/update flow — e.g. the user rebinds it in System Settings
     * (KGlobalAccel) or the compositor confirms/assigns a trigger (Portal
     * BindShortcuts Response). Consumers displaying bindings re-query
     * currentTriggers() on this signal. Backends that cannot observe
     * external changes simply never emit it.
     */
    void triggersChanged(QString id);
};

} // namespace PhosphorShortcuts
