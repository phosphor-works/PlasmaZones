// SPDX-FileCopyrightText: 2026 fuddlesworth
// SPDX-License-Identifier: GPL-3.0-or-later

#pragma once

#include "plasmazones_export.h"
#include <QObject>
#include <QString>
#include <QVariant>
#include <QTimer>
#include <functional>
#include <QHash>

#include <optional>

namespace PhosphorAnimation {
class PhosphorProfileRegistry;
}

namespace PlasmaZones {

class ISettings;
class Settings; // Forward declaration for concrete type

/**
 * @brief Settings read/write operations: the service behind
 *        org.phosphor_works.Placement.Settings
 *
 * Bound to D-Bus by SettingsAdaptor (dbus/settingsadaptor.h), which converts
 * between QDBusVariant / QDBusArgument and plain QVariant and turns a refusal
 * here into a D-Bus error.
 *
 * Uses registry pattern for setSetting.
 */
/// getSetting answers an UNKNOWN key with no value and an error message,
/// which SettingsService sends as a real D-Bus error instead of a
/// valid-but-empty value. That distinction is load-bearing: getSetting resolves keys
/// through a hand-maintained registry, not through Qt property reflection, so a setting
/// whose registration is forgotten is a live possibility — and while the miss
/// answered with an empty string, every one of the ~50 callers coerced it blind
/// (QVariant("").toBool() is false), silently shipping the default-inverted
/// value instead of failing. An error reply makes loadSettingAsync's
/// reply.isValid() false, so the callback never runs and the caller keeps its
/// own default. One central guard instead of a type-check at every call site.
class PLASMAZONES_EXPORT SettingsService : public QObject
{
    Q_OBJECT

public:
    /// @param settings Settings interface (required).
    /// @param profileRegistry Per-process motion-profile registry holding
    ///        the merged per-event `PhosphorAnimation::Profile` set (the
    ///        same registry the daemon's SurfaceAnimator path resolves
    ///        durations from). Borrowed; must outlive the adaptor.
    ///        Optional — when null, the `motionProfileTree` getter is not
    ///        registered and consumers fall back to the global duration.
    /// @param parent Qt parent (D-Bus adaptors are owned by their adapted
    ///        QObject via Qt parent-child).
    explicit SettingsService(ISettings* settings, PhosphorAnimation::PhosphorProfileRegistry* profileRegistry = nullptr,
                             QObject* parent = nullptr);
    ~SettingsService() override;

    /// Null the borrowed ISettings and profile-registry pointers, sever their
    /// signal wiring, and flush any pending debounced save. Called from
    /// EngineRuntime::stop() before the owning unique_ptr members destroy the
    /// backing objects — after detach() the adaptor's D-Bus slots hit the
    /// null-guard paths instead of dereferencing a dangling pointer, and
    /// the destructor is a no-op for the save-on-teardown branch.
    void detach();

    /// The value of @p key, or nullopt for an empty or unknown key, with the
    /// reason in @p error. A registered key whose getter answers an invalid
    /// variant answers an empty string. Not a slot: SettingsService serves
    /// it on the bus as a QDBusVariant.
    std::optional<QVariant> getSetting(const QString& key, QString* error = nullptr);
    /// Write @p value, a plain Qt value (SettingsService unwraps a
    /// QDBusArgument from the wire), to @p key. False for an empty or unknown key.
    bool setSetting(const QString& key, const QVariant& value);
    /// Write one per-screen value. False, with the reason in @p rejection,
    /// when it is refused: no backend, an implausible screen id or key, an
    /// unknown or read-only category.
    bool setPerScreenSetting(const QString& screenId, const QString& category, const QString& key,
                             const QVariant& value, QString* rejection = nullptr);
    /// Clear one category of a screen's per-screen values. False, with the
    /// reason in @p rejection, when it is refused, as setPerScreenSetting.
    bool clearPerScreenSettings(const QString& screenId, const QString& category, QString* rejection = nullptr);

public Q_SLOTS:
    // Settings operations
    void reloadSettings();
    void saveSettings();
    void resetToDefaults();

    // Generic get/set (registry-based)
    QString getAllSettings();

    /**
     * @brief Batch-get multiple settings in one D-Bus call.
     *
     * Reads every requested key via the getter registry and returns a map
     * containing only the keys that were found. An unknown key is omitted from the
     * result and logged at DEBUG level, so a caller probing for optional settings does
     * not spam production logs. Callers fall back to their own defaults for anything
     * missing.
     *
     * Exists to collapse the editor startup's per-key getSetting() chain
     * (8 round-trips for gap/overlay settings) into a single round-trip.
     *
     * @param keys List of setting keys to fetch
     * @return Map of key -> value for every key that was found
     */
    QVariantMap getSettings(const QStringList& keys);

    /**
     * @brief Batch-set multiple settings in one D-Bus call.
     *
     * Applies all values via the setter registry, saves once (synchronously), and lets
     * the config backend's change notification propagate settingsChanged.
     *
     * An unknown key makes the call return false — writing a key nobody knows is a caller
     * bug, not an optional probe — but it does not abort the batch: every key that IS known
     * still gets applied. It is logged at DEBUG, not warning: the key comes from whoever is
     * on the session bus, and a warning let any process fill the daemon's log with content
     * of its own choosing. The `false` is the real signal.
     *
     * @param settings Map of setting key -> value
     * @return true if every key was known to the registry and every key that HAS a setter
     *         applied successfully. A key that is registered read-only (motionProfileTree,
     *         animationShaderSearchPaths) is skipped without failing
     *         the batch — see the skip's rationale at the call site. This is false when a key
     *         is unknown OR when a setter rejects its value (an out-of-range enum, an invalid
     *         blob); such a setter returns false and flips the batch result.
     */
    bool setSettings(const QVariantMap& settings);

    QStringList getSettingKeys();

    // Per-screen settings (categories: "autotile", "scrolling", "snapping",
    // "zoneSelector", "scrollingZoneSelector")
    QVariantMap getPerScreenSettings(const QString& screenId, const QString& category);

    /**
     * @brief Batch-set multiple per-screen keys in one D-Bus call.
     *
     * Applies every (key, value) pair in @p values via the same category
     * dispatch as setPerScreenSetting, then schedules a single debounced
     * save. Mirrors the global getSettings/setSettings batch pattern.
     *
     * NOTE: nothing in this tree calls it. It was added for a KCM per-monitor flush that
     * never materialised — the KCM writes config.json in-process and calls
     * reloadSettings(). It stays as part of the PUBLISHED D-Bus write surface (declared in
     * org.phosphor_works.Placement.Settings.xml, covered by tests/unit/dbus), for external clients. Do
     * not re-justify it by naming an in-tree caller; there is none.
     *
     * Unknown keys inside @p values are passed through to the underlying
     * Settings method, which no-ops and logs at DEBUG (the key is caller-supplied; see
     * setSettings) — consistent with single-key behavior.
     *
     * @param screenId Virtual or physical screen identifier
     * @param category "autotile" | "scrolling" | "snapping" | "zoneSelector" | "scrollingZoneSelector". NOTE:
     * "snapping" is READ-ONLY (it projects the config's per-monitor gaps), so a write to it is rejected and does
     * nothing. Write those through "autotile".
     * @param values   Map of key -> value. QDBusArgument-wrapped values
     *                 from the wire are unwrapped via DBusVariantUtils
     *                 before reaching the setter.
     * @return true if the category was recognized and writable, and the batch ran; false if
     *         the category was unknown, or is the read-only "snapping" projection. (There is
     *         no longer any concrete-backend check — the dispatch runs against ISettings.)
     *         Per-key failures are logged by the underlying Settings but
     *         cannot be surfaced here since the setters return void.
     */
    bool setPerScreenSettings(const QString& screenId, const QString& category, const QVariantMap& values);

    /**
     * @brief Asynchronous window list request (fire-and-forget).
     *
     * Emits runningWindowsRequested() so the KWin effect enumerates its
     * window list and sends it back via provideRunningWindows(). The
     * resulting JSON is then broadcast via the runningWindowsAvailable
     * signal — subscribers update their view when the signal arrives
     * rather than blocking on the D-Bus call. This is the only
     * supported path; the old blocking getRunningWindows() method was
     * removed in Phase 6 of refactor/dbus-performance.
     *
     * Returns immediately. Safe to call repeatedly from UI — each call
     * triggers at most one KWin round-trip; duplicate requests while a
     * previous response is in flight are coalesced by the effect side.
     */
    void requestRunningWindows();

    /**
     * @brief Receive window list from KWin effect (callback)
     * @param json JSON array of window rows, shaped as the provideRunningWindows
     *        docstring in org.phosphor_works.Placement.Settings.xml gives it. A payload over
     *        PhosphorProtocol::Service::MaxRunningWindowsPayloadBytes is refused.
     */
    void provideRunningWindows(const QString& json);

    /**
     * @brief Add one of the host's own settings (HostSettingsExtension) to the
     *        registry under @p name, for getSetting, setSetting, setSettings and
     *        the schema calls alike
     *
     * @p get and @p set are the host's, and @p set answers whether the value
     * was accepted. @p group and @p key are where the value is stored, so a
     * batch announces its change as ISettings::hostValueChanged the way a
     * direct write does. The registry is rebuilt with the service, so a host
     * registers again after every EngineRuntime::init().
     */
    void registerHostSetting(const QString& name, const QString& type, const QString& group, const QString& key,
                             std::function<QVariant()> get, std::function<bool(const QVariant&)> set);

    /**
     * @brief Get metadata for a single setting
     * @param key Setting key name
     * @return JSON: {key, type} — type is "bool"|"int"|"double"|"string"|"color"|"themeColor"|"stringlist"|"map"
     */
    QString getSettingSchema(const QString& key);

    /**
     * @brief Get metadata for all settings
     * @return JSON object: {key: {type}, ...}
     */
    QString getAllSettingSchemas();

Q_SIGNALS:
    void settingsChanged();

    /**
     * @brief Daemon → layout editor and Settings app: an Editor.* setting
     * changed on a daemon reload (a settings-app Save, the editor's own
     * write coming back, or a factory reset over D-Bus).
     *
     * Kept off @ref settingsChanged because nothing else reads those keys,
     * and every subscriber of settingsChanged (the daemon's own handlers, the
     * KWin effect, the shell) would otherwise re-fetch its whole state each
     * time the editor saved a slider.
     */
    void editorSettingsChanged();

    /**
     * @brief Daemon → KWin effect: the per-event motion-profile tree changed.
     *
     * Emitted when the motion-profile registry mutates (a per-event
     * `profiles/<path>.json` override was edited and the daemon
     * rescanned it).
     * Deliberately separate from @ref settingsChanged: the Settings app
     * listens only to settingsChanged, so routing registry mutations
     * here keeps the app's value-change / save-discard detection from
     * being reset by its own immediate profile-file writes. The kwin
     * effect subscribes to this signal and re-fetches `motionProfileTree`.
     */
    void motionProfileTreeChanged();

    /**
     * @brief Daemon → KWin effect: the session went idle, or came back.
     *
     * The effect pauses decoration-chain animation while idle. That is the only
     * thing that lets the GPU leave its top performance state: an animated pack
     * repaints every window carrying it on every vsync, and it is the existence of
     * per-frame work — not its size — that holds the clocks up, so making each
     * frame cheaper cannot recover the idle power. Not drawing can.
     *
     * The daemon owns the detection because idleness is a Wayland CLIENT concern
     * (`ext-idle-notify-v1`, via PhosphorServiceIdle) and the effect lives inside
     * the compositor, which serves that protocol rather than consuming it. The
     * effect only ever sees the resolved boolean.
     */
    void sessionIdleChanged(bool idle);

    /**
     * @brief Daemon → KWin effect: please enumerate running windows.
     *
     * Emitted by requestRunningWindows(). The effect answers by calling
     * provideRunningWindows(), which then broadcasts runningWindowsAvailable
     * to all subscribers.
     */
    void runningWindowsRequested();

    /**
     * @brief Daemon → clients: fresh running-windows JSON is available.
     *
     * Emitted every time provideRunningWindows() receives a payload from
     * the KWin effect. Subscribers (SettingsController) cache the value
     * and present it to the UI without a blocking round-trip.
     *
     * @param json JSON array: [{windowClass, rawWindowClass, appName, caption,
     *        desktopFile}, ...], as the runningWindowsAvailable docstring in
     *        org.phosphor_works.Placement.Settings.xml describes it.
     */
    void runningWindowsAvailable(const QString& json);

private:
    void initializeRegistry();
    /// The per-mode slices of the registry, split into
    /// settingsservice_registry_snapping.cpp,
    /// settingsservice_registry_autotile.cpp and
    /// settingsservice_registry_scrolling.cpp for file size. Each is called
    /// only by initializeRegistry and fills the same getter / setter / schema
    /// maps; which key lives in which slice follows the boundary rule
    /// documented in those files' banners.
    void initializeRegistrySnapping();
    void initializeRegistryAutotile();
    void initializeRegistryScrolling();

    /**
     * @brief Schedule a debounced save
     *
     * Performance optimization: Batches multiple setting changes into a single
     * save operation instead of writing to disk on every change.
     */
    void scheduleSave();

    ISettings* m_settings; // Interface type (DIP)
    PhosphorAnimation::PhosphorProfileRegistry* m_profileRegistry = nullptr; ///< Borrowed; outlives adaptor

    // Registry pattern
    using Getter = std::function<QVariant()>;
    using Setter = std::function<bool(const QVariant&)>;

    QHash<QString, Getter> m_getters;
    QHash<QString, Setter> m_setters;
    /// registerHostSetting's names → their (group, key), for a batch's replay.
    QHash<QString, std::pair<QString, QString>> m_hostSettingKeys;
    QHash<QString, QString>
        m_schemas; // key -> type ("bool"|"int"|"double"|"string"|"color"|"themeColor"|"stringlist"|"map")

    // Debounced save timer (performance optimization)
    QTimer* m_saveTimer = nullptr;
    static constexpr int SaveDebounceMs = 500; // 500ms debounce

    // Debounced motion-profile-tree change notifier. A single
    // installMotionProfileTree calls reloadFromOwner(), which emits one
    // profileChanged per touched path plus a closing ownerReloaded —
    // N+1 registry signals for one logical change. Without coalescing,
    // each would emit motionProfileTreeChanged() and the kwin-effect
    // would re-fetch + re-parse the whole tree N+1 times. This timer
    // collapses the burst into a single emission.
    QTimer* m_motionTreeNotifyTimer = nullptr;
    static constexpr int MotionTreeNotifyDebounceMs = 50;

    /// Memoized JSON array of animation-shader search paths. A member, not a
    /// function-local static: as a static it lived for the whole process, so a
    /// mid-session XDG_DATA_HOME change was never picked up and it survived
    /// detach(), which could not clear it. Empty means "not yet resolved".
    QString m_cachedShaderSearchPaths;
};

} // namespace PlasmaZones
