// SPDX-FileCopyrightText: 2026 fuddlesworth
// SPDX-License-Identifier: GPL-3.0-or-later

#include "shortcutmanager.h"
#include "shortcutmanager_ids.h"
#include "shortcutmanager_table.h"

#include "config/configdefaults.h"
#include "config/settings.h"
#include "core/platform/logging.h"
#include "phosphor_i18n.h"

#include <PhosphorShortcuts/Factory.h>
#include <PhosphorShortcuts/IBackend.h>
#include <PhosphorShortcuts/Registry.h>

#include <QHash>
#include <QSet>
#include <QStringList>
#include <QTimer>

#include <algorithm>
#include <iterator>

namespace PlasmaZones {

namespace {

// Shortcut ids + indexed-slot id helpers live in shortcutmanager_ids.h (see
// its header comment for every TU that reads them).
using namespace ShortcutIds;
using ShortcutTable::StaticEntry;

// The workspace slot families are registered from the indexed loops below,
// but their config keys are bounded by the settings-side constant. Tie the
// two together so raising one without the other fails to compile rather than
// silently registering a slot the config cannot store.
static_assert(kIndexedSlotCount == ConfigDefaults::WorkspaceSlotCount,
              "workspace slot families are registered with kIndexedSlotCount; keep it equal to the config bound");

// Indexed slot defaults — array of static accessors so the slot loop can
// resolve defaults by index without one constexpr-if branch per slot. Sized
// by their initializers, not by kIndexedSlotCount: a declared bound would
// value-initialize any slot left out to a null getter, which the slot loop
// would then call. The static_asserts below catch a missing or extra slot.
using DefaultGetter = QString (*)();
constexpr DefaultGetter kQuickLayoutDefaults[] = {
    &ConfigDefaults::quickLayout1Shortcut, &ConfigDefaults::quickLayout2Shortcut, &ConfigDefaults::quickLayout3Shortcut,
    &ConfigDefaults::quickLayout4Shortcut, &ConfigDefaults::quickLayout5Shortcut, &ConfigDefaults::quickLayout6Shortcut,
    &ConfigDefaults::quickLayout7Shortcut, &ConfigDefaults::quickLayout8Shortcut, &ConfigDefaults::quickLayout9Shortcut,
};
constexpr DefaultGetter kSnapToZoneDefaults[] = {
    &ConfigDefaults::snapToZone1Shortcut, &ConfigDefaults::snapToZone2Shortcut, &ConfigDefaults::snapToZone3Shortcut,
    &ConfigDefaults::snapToZone4Shortcut, &ConfigDefaults::snapToZone5Shortcut, &ConfigDefaults::snapToZone6Shortcut,
    &ConfigDefaults::snapToZone7Shortcut, &ConfigDefaults::snapToZone8Shortcut, &ConfigDefaults::snapToZone9Shortcut,
};
constexpr DefaultGetter kScrollFocusTabDefaults[] = {
    &ConfigDefaults::scrollFocusTab1Shortcut, &ConfigDefaults::scrollFocusTab2Shortcut,
    &ConfigDefaults::scrollFocusTab3Shortcut, &ConfigDefaults::scrollFocusTab4Shortcut,
    &ConfigDefaults::scrollFocusTab5Shortcut, &ConfigDefaults::scrollFocusTab6Shortcut,
    &ConfigDefaults::scrollFocusTab7Shortcut, &ConfigDefaults::scrollFocusTab8Shortcut,
    &ConfigDefaults::scrollFocusTab9Shortcut,
};
static_assert(std::size(kQuickLayoutDefaults) == kIndexedSlotCount, "one quick-layout default per slot");
static_assert(std::size(kSnapToZoneDefaults) == kIndexedSlotCount, "one snap-to-zone default per slot");
static_assert(std::size(kScrollFocusTabDefaults) == kIndexedSlotCount, "one focus-tab default per slot");

// QKeySequence(QString) silently returns an empty sequence on malformed
// input. Wrap with a warning log so a typo in the config surfaces from the
// logs instead of silently disabling a shortcut. The warning latches per
// (id, spelling): the currentSeq lambdas re-parse every entry on every
// settings save, and a persistently malformed value would otherwise re-log
// the identical line for the life of the process.
QKeySequence parseSequence(const QString& raw, const QString& contextId)
{
    if (raw.isEmpty()) {
        return {};
    }
    QKeySequence seq(raw);
    if (seq.isEmpty()) {
        // Main-thread only: the warn-once latch's insert is not synchronized
        // (zero-init of the static is thread-safe, the mutation is not), and
        // every current caller runs on the daemon main thread.
        static QHash<QString, QString> warnedSpellings;
        if (warnedSpellings.value(contextId) != raw) {
            warnedSpellings.insert(contextId, raw);
            qCWarning(lcShortcuts) << "Failed to parse shortcut sequence for" << contextId << ":" << raw;
        }
    }
    return seq;
}

} // namespace

ShortcutManager::ShortcutManager(Settings* settings, QObject* parent)
    : QObject(parent)
    , m_settings(settings)
{
    Q_ASSERT(settings);
    if (!settings) {
        // Release-build pair for the assert above. Every Entry::currentSeq
        // lambda captures this pointer raw, so a built table would deref null
        // on its first rebind. The manager instead stays inert: the guard at
        // the top of registerShortcuts() keeps m_entries empty, and every
        // other public entry point already no-ops on an empty table or a null
        // m_registry.
        qCCritical(lcShortcuts) << "ShortcutManager constructed without Settings — shortcuts disabled";
        return;
    }

    // Backend + Registry are NOT created here — they're created lazily in
    // registerShortcuts() so unregisterShortcuts() can fully release the
    // Portal session (XDG GlobalShortcuts has no per-id release; the only
    // way to drop grabs is to destroy the session). Re-creating in a
    // subsequent registerShortcuts() then goes through a fresh
    // CreateSession round-trip and the compositor sees clean state.
    // This matches the IBackend lifecycle documented on
    // PortalBackend::unregisterShortcut.

    // Settings::settingsChanged() fires both on individual setter emits AND
    // on bulk load (KCM reload). A single connect handles both cases; the
    // Registry internally no-ops rebinds that don't change the sequence, so
    // there's no cost to the full-table refresh pattern.
    connect(m_settings, &Settings::settingsChanged, this, &ShortcutManager::updateShortcuts);
}

ShortcutManager::~ShortcutManager() = default;

// The null branch is defence, not a live path: registerShortcuts() returns
// early without m_settings and nothing nulls it afterwards, so a fire lambda
// can only run with it set. Kept because these run from a callback the
// backend owns, and a default step beats a crash if that ever stops holding.
int ShortcutManager::scrollColumnWidthStepPercent() const
{
    return m_settings ? m_settings->scrollingColumnWidthStepPercent()
                      : ConfigDefaults::scrollingColumnWidthStepPercent();
}

int ShortcutManager::scrollWindowHeightStepPercent() const
{
    return m_settings ? m_settings->scrollingWindowHeightStepPercent()
                      : ConfigDefaults::scrollingWindowHeightStepPercent();
}

int ShortcutManager::scrollViewScrollStepPercent() const
{
    return m_settings ? m_settings->scrollingViewScrollStepPercent() : ConfigDefaults::scrollingViewScrollStepPercent();
}

void ShortcutManager::registerShortcuts()
{
    if (!m_settings) {
        // Constructed without Settings. buildEntries() would produce currentSeq
        // lambdas over a null pointer and the bind loop below would deref them,
        // so stop before the backend is even created — that keeps the ctor's
        // "shortcuts disabled" claim true instead of trading one crash site for
        // a Portal round-trip and the same crash.
        qCWarning(lcShortcuts) << "registerShortcuts(): no Settings — shortcuts stay disabled";
        return;
    }
    if (m_registrationInProgress) {
        qCWarning(lcShortcuts) << "registerShortcuts() called re-entrantly — ignoring";
        return;
    }
    if (!m_entries.isEmpty()) {
        // Already registered. A second registerShortcuts() call would
        // rebuild m_entries via buildEntries() (which clears the local
        // table) without first unbinding the prior set from the Registry,
        // leaving orphan registry entries pointing at stale callback
        // captures. The intended single-call lifecycle is enforced here
        // — call unregisterShortcuts() first if you want to rebuild.
        qCWarning(lcShortcuts) << "registerShortcuts(): already registered (" << m_entries.size()
                               << "entries) — call unregisterShortcuts() first to rebuild";
        return;
    }

    // Lazy backend creation. Lets unregisterShortcuts() fully drop the
    // Portal session and a subsequent re-register start fresh. unique_ptr
    // owns lifetime — DON'T also pass `this` as Qt parent, or the
    // backend/registry end up with two owners.
    if (!m_backend) {
        m_backend = PhosphorShortcuts::createBackend(PhosphorShortcuts::BackendHint::Auto, nullptr);
    }
    if (!m_registry) {
        m_registry = std::make_unique<PhosphorShortcuts::Registry>(m_backend.get(), nullptr);
        // External rebinds (System Settings, compositor-side) invalidate the
        // cheatsheet catalog's trigger strings. Registry filters to owned ids;
        // this narrows further to the ids the catalog lists, so a rebind of
        // a transient grab (the picker's arrows, an Escape) does not
        // republish an open sheet.
        connect(m_registry.get(), &PhosphorShortcuts::Registry::triggersChanged, this, [this](const QString& id) {
            const bool catalogued =
                isCataloguedAdhocId(id) || std::any_of(m_entries.cbegin(), m_entries.cend(), [&id](const Entry& e) {
                    return e.id == id;
                });
            if (catalogued) {
                Q_EMIT cheatsheetModelChanged();
            }
        });
    }

    m_registrationInProgress = true;

    buildEntries();
    purgeStaleRecords();

    // Whether a park right after registration actually gives the grab back.
    // Portal cannot release a grab per id, so on it a parked entry is never
    // registered in the first place.
    const bool parkReleasesGrab = m_backend->suspendReleasesGrab();
    for (const auto& e : std::as_const(m_entries)) {
        m_registry->bind(e.id, e.defaultSeq, e.description, e.fire);
        // Apply current sequence from settings after initial bind so the
        // first flush reflects the user's saved preference, not only the
        // compiled-in default.
        const QKeySequence seq = e.currentSeq();
        m_registry->rebind(e.id, seq);
        if (entryActive(e, seq)) {
            continue;
        }
        if (parkReleasesGrab) {
            // Register the entry first and park it on the settle, rather than
            // never registering it at all. Registration is what puts a bound
            // id into kglobalshortcutsrc, and that record is what makes it a
            // rebindable row in the System Settings Shortcuts module, so a
            // user running with the feature off can still adjust the chords
            // there. An id with no chord stays unregistered either way (an
            // empty grab is the discussion #155 hazard); the settings app's
            // shortcut recorders are where those are assigned. The entry
            // holds its grab until the settle, which on KGlobalAccel is the
            // same event-loop turn, since its flush answers ready() at once.
            m_settingsDirty = true;
        } else {
            m_registry->setActive(e.id, false);
        }
    }

    // Tag this batch so a settle that arrives late (either path) can tell
    // whether it still refers to the batch it was armed for.
    const quint64 generation = ++m_registrationGeneration;

    connect(
        m_registry.get(), &PhosphorShortcuts::Registry::ready, this,
        [this, generation] {
            settleRegistration(generation);
        },
        Qt::SingleShotConnection);

    // Fallback settle: a backend that never answers (Portal request lost, a
    // compositor that drops the reply) would otherwise leave
    // m_registrationInProgress true forever — every later settings rebind
    // silently defers and every adhoc grab queues undrained, which kills the
    // picker / snap-assist Escape binding for the rest of the session.
    QTimer::singleShot(kRegistrationSettleTimeoutMs, this, [this, generation] {
        if (generation != m_registrationGeneration || !m_registrationInProgress) {
            // Superseded by a later registerShortcuts()/unregisterShortcuts(),
            // or already settled by ready(). Settling here would clear the
            // in-flight flag of a DIFFERENT, still-pending batch.
            return;
        }
        qCWarning(lcShortcuts) << "Shortcut backend never reported ready after" << kRegistrationSettleTimeoutMs
                               << "ms — settling registration anyway";
        settleRegistration(generation);
    });

    m_registry->flush();
}

void ShortcutManager::settleRegistration(quint64 generation)
{
    if (generation != m_registrationGeneration || !m_registrationInProgress) {
        // Stale (a newer batch owns the manager) or already settled. Running
        // the body again would re-publish the catalog for no change, which
        // the cheatsheet re-pushes as visible churn.
        return;
    }
    m_registrationInProgress = false;
    qCInfo(lcShortcuts) << "Registered" << m_entries.size() << "shortcuts";
    // A settle with BOTH dirty settings and queued adhoc ops coalesces onto
    // ONE backend flush (see applyShortcutUpdates' doc): the rebinds stay
    // pending, the drain's trailing flush carries them, and only when
    // nothing was drained does the deferred flush run here. The
    // cheatsheetModelChanged for the rebinds is emitted AFTER whichever
    // flush carried them — the model reads the backend's read-back
    // (effectiveTriggers prefers it), so an emit ahead of the flush would
    // publish the pre-flush sequences and nothing re-emits until the next
    // settings save.
    bool modelEmitted = false;
    if (m_settingsDirty) {
        m_settingsDirty = false;
        const bool rebound = applyShortcutUpdates(/*deferFlush=*/true);
        // Replay any adhoc (un)registrations that arrived while the initial
        // batch was in flight. Must run AFTER the in-progress flag is
        // cleared so each drained op takes the immediate path instead of
        // re-queuing itself.
        const bool drainFlushed = drainPendingAdhocOps();
        if (rebound && !drainFlushed) {
            m_registry->flush();
        }
        if (rebound) {
            Q_EMIT cheatsheetModelChanged();
            modelEmitted = true;
        }
    } else {
        drainPendingAdhocOps();
    }
    // The catalog is first meaningful once the batch has settled (backend
    // read-back can answer now). One emit per settle: the dirty-settings
    // arm above may already have pushed it post-flush.
    if (!modelEmitted) {
        Q_EMIT cheatsheetModelChanged();
    }
}

void ShortcutManager::updateShortcuts()
{
    applyShortcutUpdates(/*deferFlush=*/false);
}

void ShortcutManager::setWorkspaceFamilyActive(bool active)
{
    if (m_workspaceFamilyActive == active) {
        return;
    }
    m_workspaceFamilyActive = active;
    // Deferred like any settings save while the first batch is in flight.
    applyShortcutUpdates(/*deferFlush=*/false);
}

bool ShortcutManager::applyShortcutUpdates(bool deferFlush)
{
    if (m_registrationInProgress) {
        // Defer — the ready() callback above will call us again.
        m_settingsDirty = true;
        return false;
    }
    if (m_entries.isEmpty()) {
        return false;
    }
    // settingsChanged fires for every settings save; only a save that
    // actually moved a shortcut sequence needs a backend flush or a
    // cheatsheet refresh (a visible sheet re-pushes its model on the emit,
    // so over-notifying is user-visible churn, not just wasted work).
    if (!rebindAll()) {
        return false;
    }
    if (!deferFlush) {
        m_registry->flush();
        Q_EMIT cheatsheetModelChanged();
    }
    // Deferred: the CALLER owns both the flush and the post-flush emit —
    // emitting here would publish the pre-flush backend read-back.
    return true;
}

void ShortcutManager::setBackendForTesting(std::unique_ptr<PhosphorShortcuts::IBackend> backend)
{
    Q_ASSERT_X(m_entries.isEmpty(), "setBackendForTesting", "inject before registerShortcuts()");
    if (!m_entries.isEmpty()) {
        // Release-build pair for the assert. Resetting the registry while
        // m_entries stays populated would break the "entries non-empty
        // implies registry present" invariant updateShortcuts() relies on —
        // the next settings save would deref a null m_registry in
        // rebindAll(). Release the live registration first so the invariant
        // survives the misuse.
        qCWarning(lcShortcuts) << "setBackendForTesting() called after registerShortcuts() — releasing the live "
                                  "registration before installing the injected backend";
        unregisterShortcuts();
    }
    m_registry.reset();
    m_backend = std::move(backend);
}

void ShortcutManager::unregisterShortcuts()
{
    m_registrationInProgress = false;
    m_settingsDirty = false;
    // Supersede any armed fallback settle: its generation no longer matches.
    ++m_registrationGeneration;
    // Adhoc ops queued behind an in-flight initial batch belong to UI
    // contexts (e.g. an overlay's Escape grab) that no longer exist after
    // teardown; replaying them on the next registerShortcuts() would
    // re-bind a stale grab.
    m_pendingAdhocOps.clear();
    // Deliberately NO Registry::unbind() sweep over m_entries: unbind routes
    // to IBackend::unregisterShortcut, and on KGlobalAccel that purges the
    // id's persistent kglobalshortcutsrc record — the store where the user's
    // System Settings customisations live. This runs on the daemon stop()
    // path (SIGTERM included), so the sweep deleted every customised binding
    // on each service restart (discussion #851). Destroying the backend below
    // releases the grabs without touching persistent records: each QAction's
    // destruction makes the KGlobalAccel client send setInactive (the grab
    // goes, the record stays), the backend destructor purges only transient
    // ids, and PortalBackend releases everything by closing the session.
    //
    // Deliberately NO cheatsheetModelChanged emit, unlike every other
    // catalog-changing path. The two callers are the daemon stop() path —
    // where the only consumer (refreshCheatsheetIfVisible) is being torn
    // down alongside the overlay service — and setBackendForTesting's
    // misuse branch, which is a programming-error recovery that no live
    // overlay should ever observe; neither needs an empty-catalog announce.
    m_entries.clear();

    // Tear down the backend so any Portal session is closed and grabs are
    // released compositor-side. KGlobalAccel and DBusTrigger backends are
    // cheap to recreate; PortalBackend's CreateSession is a single async
    // round-trip that's hidden by its existing m_flushRequested deferral.
    // A subsequent registerShortcuts() will lazily recreate both.
    m_registry.reset();
    m_backend.reset();
}

bool ShortcutManager::setForeignShortcuts(const QString& componentName, const QString& actionName,
                                          const QList<QKeySequence>& sequences)
{
    return m_registry ? m_registry->setForeignShortcuts(componentName, actionName, sequences) : false;
}

std::optional<QList<QKeySequence>> ShortcutManager::foreignShortcuts(const QString& componentName,
                                                                     const QString& actionName) const
{
    // No registry is a FAILED query, not an unbound action — nullopt, so the
    // caller keeps its hands off the chord.
    return m_registry ? m_registry->foreignShortcuts(componentName, actionName) : std::nullopt;
}

bool ShortcutManager::entryActive(const Entry& e, const QKeySequence& seq) const
{
    if (!e.workspaceGated) {
        return true;
    }
    // The workspace family holds a grab only while a workspace controller
    // runs: an inert system-wide grab shadows those chords for every other
    // application.
    if (!m_workspaceFamilyActive) {
        return false;
    }
    // With the stock takeover off, KWin still owns its "Switch One Desktop
    // Up/Down" chords, and kglobalacceld dispatches a key to the shortcut
    // registered first, which is KWin's. The focus pair would hold a grab
    // that never fires while the sheet advertised it, so it parks until the
    // user moves it to a chord of its own or turns the takeover on.
    if (e.sharesKWinStockChord && seq == e.defaultSeq && m_settings && !m_settings->workspacesRebindKWinShortcuts()) {
        return false;
    }
    return true;
}

bool ShortcutManager::rebindAll()
{
    bool anyChanged = false;
    for (const auto& e : std::as_const(m_entries)) {
        const QKeySequence seq = e.currentSeq();
        if (m_registry->shortcut(e.id) != seq) {
            anyChanged = true;
        }
        m_registry->rebind(e.id, seq);
        // Parked through Registry::setActive rather than by rebinding to an
        // empty sequence, because the empty-sequence path unregisters on the
        // backend and KGlobalAccel's unregister wipes the user's saved chords
        // for the whole family. A chord changed in the same pass as the park
        // is still written through to the platform's record by the next
        // flush (Registry::setActive). The active flag counts towards
        // anyChanged: a pass that only flipped a gate moves no sequence, and
        // without this the flush that applies the park would never run.
        const bool active = entryActive(e, seq);
        if (m_registry->isActive(e.id) != active) {
            anyChanged = true;
        }
        m_registry->setActive(e.id, active);
    }
    return anyChanged;
}

void ShortcutManager::purgeStaleRecords()
{
    const std::optional<QStringList> stored = m_registry->storedShortcutIds();
    if (!stored) {
        // Not a backend with records of its own, or the query failed. A
        // failed query leaves leftovers in place until the next start, which
        // is no worse than not looking.
        return;
    }
    QSet<QString> keep;
    keep.reserve(m_entries.size());
    for (const auto& e : std::as_const(m_entries)) {
        keep.insert(e.id);
    }
    for (const QString& id : *stored) {
        if (!keep.contains(id)) {
            qCInfo(lcShortcuts) << "Removing stale shortcut record" << id;
            m_registry->purge(id);
        }
    }
}

QStringList ShortcutManager::staticShortcutIds()
{
    const std::size_t count = ShortcutTable::staticEntryCount();
    const StaticEntry* const table = ShortcutTable::staticEntries();
    QStringList ids;
    ids.reserve(static_cast<int>(count));
    for (std::size_t i = 0; i < count; ++i) {
        ids.append(QString::fromLatin1(table[i].id));
    }
    return ids;
}

void ShortcutManager::buildEntries()
{
    const std::size_t staticCount = ShortcutTable::staticEntryCount();
    const StaticEntry* const staticTable = ShortcutTable::staticEntries();

    m_entries.clear();
    m_entries.reserve(static_cast<int>(staticCount) + 5 * kIndexedSlotCount);

    Settings* s = m_settings;
    ShortcutManager* sm = this;

    // Static table (shortcutmanager_table.cpp) — one entry per row.
    for (std::size_t i = 0; i < staticCount; ++i) {
        const StaticEntry& src = staticTable[i];
        Entry e;
        e.id = QString::fromLatin1(src.id);
        e.defaultSeq = parseSequence(src.defGetter(), e.id);
        e.description = PhosphorI18n::tr(src.label);
        const auto curGetter = src.curGetter;
        const QString idCopy = e.id;
        e.currentSeq = [s, curGetter, idCopy] {
            return parseSequence((s->*curGetter)(), idCopy);
        };
        const auto fire = src.fire;
        e.fire = [sm, fire] {
            fire(sm);
        };
        e.workspaceGated = src.workspaceGated;
        e.sharesKWinStockChord =
            e.id == QLatin1String(kIdWorkspaceFocusUp) || e.id == QLatin1String(kIdWorkspaceFocusDown);
        m_entries.push_back(std::move(e));
    }

    // Quick layout slots. Default getters indexed via kQuickLayoutDefaults;
    // current getter is Settings::quickLayoutShortcut(int) keyed by slot index.
    for (int i = 0; i < kIndexedSlotCount; ++i) {
        Entry e;
        e.id = quickLayoutId(i);
        e.defaultSeq = parseSequence(kQuickLayoutDefaults[i](), e.id);
        e.description = PhosphorI18n::tr("Apply Layout %1").arg(i + 1);
        const QString idCopy = e.id;
        e.currentSeq = [s, i, idCopy] {
            return parseSequence(s->quickLayoutShortcut(i), idCopy);
        };
        const int slot = i + 1;
        e.fire = [this, slot] {
            Q_EMIT quickLayoutRequested(slot);
        };
        m_entries.push_back(std::move(e));
    }

    // Workspace quick-shortcut slots. The slot's target workspace and its
    // chord are both assigned on the settings app's workspace shortcuts page.
    // The family ships unbound (ConfigDefaults::workspaceMoveSlotShortcut says
    // why), and an unbound id is never registered with a key-grabbing backend,
    // so the settings page is where these chords are first set.
    for (int i = 0; i < kIndexedSlotCount; ++i) {
        Entry e;
        e.id = workspaceMoveSlotId(i);
        e.defaultSeq = parseSequence(ConfigDefaults::workspaceMoveSlotShortcut(), e.id);
        e.workspaceGated = true;
        // "Slot", not a bare number: this is what KGlobalAccel shows in the
        // Shortcuts KCM, and the chord does NOT move the window to the Nth
        // workspace. It moves it to whichever named workspace the settings
        // app assigned to slot N, the quick-layout model.
        e.description = PhosphorI18n::tr("Move Window to Workspace Slot %1", "single workspace slot number").arg(i + 1);
        const QString idCopy = e.id;
        e.currentSeq = [s, i, idCopy] {
            return parseSequence(s->workspaceMoveSlotShortcut(i), idCopy);
        };
        const int slot = i + 1;
        e.fire = [this, slot] {
            Q_EMIT workspaceMoveSlotRequested(slot);
        };
        m_entries.push_back(std::move(e));
    }

    // Workspace focus slots: switch the acting monitor to workspace N of its
    // own list. Unbound by default (ConfigDefaults::workspaceFocusSlotShortcut
    // says why), so like the move slots above they are first bound on the
    // settings app's workspace shortcuts page.
    for (int i = 0; i < kIndexedSlotCount; ++i) {
        Entry e;
        e.id = workspaceFocusSlotId(i);
        e.defaultSeq = parseSequence(ConfigDefaults::workspaceFocusSlotShortcut(), e.id);
        e.workspaceGated = true;
        e.description = PhosphorI18n::tr("Focus Workspace %1", "single workspace slot number").arg(i + 1);
        const QString idCopy = e.id;
        e.currentSeq = [s, i, idCopy] {
            return parseSequence(s->workspaceFocusSlotShortcut(i), idCopy);
        };
        const int slot = i + 1;
        e.fire = [this, slot] {
            Q_EMIT workspaceFocusSlotRequested(slot);
        };
        m_entries.push_back(std::move(e));
    }

    // Snap-to-zone slots. Mirror of quick-layout slots — separate signal,
    // separate Settings getter, but identical structure.
    for (int i = 0; i < kIndexedSlotCount; ++i) {
        Entry e;
        e.id = snapToZoneId(i);
        e.defaultSeq = parseSequence(kSnapToZoneDefaults[i](), e.id);
        e.description = PhosphorI18n::tr("Snap to Zone %1").arg(i + 1);
        const QString idCopy = e.id;
        e.currentSeq = [s, i, idCopy] {
            return parseSequence(s->snapToZoneShortcut(i), idCopy);
        };
        const int zoneNumber = i + 1;
        e.fire = [this, zoneNumber] {
            Q_EMIT snapToZoneRequested(zoneNumber);
        };
        m_entries.push_back(std::move(e));
    }

    // Scrolling tab ordinals, the same shape as the slot families above.
    // Every slot's default is empty, which parseSequence maps to an unbound
    // entry rather than a warning (it short-circuits on an empty string).
    for (int i = 0; i < kIndexedSlotCount; ++i) {
        Entry e;
        e.id = scrollFocusTabId(i);
        e.defaultSeq = parseSequence(kScrollFocusTabDefaults[i](), e.id);
        e.description = PhosphorI18n::tr("Focus Tab %1", "single tab ordinal").arg(i + 1);
        const QString idCopy = e.id;
        e.currentSeq = [s, i, idCopy] {
            return parseSequence(s->scrollFocusTabShortcut(i), idCopy);
        };
        const int ordinal = i + 1;
        e.fire = [this, ordinal] {
            Q_EMIT scrollFocusTabRequested(ordinal);
        };
        m_entries.push_back(std::move(e));
    }
}

} // namespace PlasmaZones
