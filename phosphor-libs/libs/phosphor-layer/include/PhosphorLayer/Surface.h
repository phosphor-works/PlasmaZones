// SPDX-FileCopyrightText: 2026 fuddlesworth
// SPDX-License-Identifier: LGPL-2.1-or-later

#pragma once

#include <PhosphorLayer/SurfaceConfig.h>
#include <PhosphorLayer/phosphorlayer_export.h>

#include <QObject>
#include <QString>

#include <memory>

QT_BEGIN_NAMESPACE
class QQuickWindow;
QT_END_NAMESPACE

namespace PhosphorLayer {

class ILayerShellTransport;
class IQmlEngineProvider;
class IScreenProvider;
class ISurfaceAnimator;
class ITransportHandle;
class SurfaceFactory;

/**
 * @brief Bundle of dependencies the Surface inherits from the factory.
 *
 * Kept in the public header because SurfaceFactory::create() returns a
 * Surface* and the dependency pointers need to travel with it; private
 * because only SurfaceFactory constructs Surfaces. No stable ABI guarantee
 * between library versions — revisit if Surface becomes user-constructible.
 *
 * @note **Pre-1.0 ABI.** SurfaceDeps is a plain aggregate exposed across
 * the DSO boundary. Adding or reordering fields is a binary-incompatible
 * change until the library reaches 1.0; consumers that aggregate-init by
 * position must rebuild. Use named-member init
 * (`SurfaceDeps{.transport = ...}`) for forward-compatibility.
 */
struct SurfaceDeps
{
    ILayerShellTransport* transport = nullptr;
    IQmlEngineProvider* engineProvider = nullptr; // Optional — nullptr = per-surface engine for contentUrl content
    /// Screen provider the Surface subscribes to for hot-unplug detection
    /// and its primary-screen fallback. Optional: without it the Surface
    /// still follows its bound screen's destruction but cannot fall back to a
    /// primary. The factory always sets it.
    IScreenProvider* screenProvider = nullptr;
    /// Phase-5 hook for show/hide visual transitions. Optional: nullptr means
    /// the Surface uses the library-internal NoOp default (synchronous
    /// onComplete; identical pre-Phase-5 behaviour). The factory propagates
    /// SurfaceFactory::Deps::animator into this slot.
    ///
    /// **Lifetime contract**: when non-null, the animator MUST outlive every
    /// Surface that holds this SurfaceDeps. The Surface dispatches through
    /// the raw pointer on every show/hide and on its own destruction
    /// (~Impl calls `animator().cancel(this)` to drop in-flight tracking).
    /// Dropping the animator while a Surface still holds the pointer is UB.
    ISurfaceAnimator* animator = nullptr;
};

/**
 * @brief One layer-shell surface with a managed lifecycle.
 *
 * Owns its QQuickWindow and, for contentUrl content loaded without a shared
 * or provider engine, its QQmlEngine. Constructed only via SurfaceFactory so
 * injected dependencies can travel with it.
 *
 * ## Lifecycle (state machine)
 *
 * | From           | Event                    | To            |
 * |----------------|--------------------------|---------------|
 * | Constructed    | warmUp()                 | Warming       |
 * | Constructed    | show()                   | Warming       |
 * | Warming        | (content ready)          | Hidden        |
 * | Hidden         | (intent=show pending)    | Shown         |
 * | Warming        | (content error)          | Failed        |
 * | Hidden         | show()                   | Shown         |
 * | Shown          | hide()                   | Hidden        |
 * | any            | (transport rejected)     | Failed        |
 *
 * A first show() therefore emits stateChanged three times, Warming, then
 * Hidden, then Shown: the surface attaches hidden and only then maps.
 *
 * Invalid transitions (e.g. `show()` from Failed) log `qCWarning` with the
 * debugName and no-op. They never throw.
 *
 * Topology-driven respawn (screen removal / compositor restart) is the
 * consumer's responsibility, not the library's: subscribe to
 * TopologyCoordinator callbacks or use ScreenSurfaceRegistry::syncToScreens
 * to decide when to destroy and rebuild surfaces. Surface observes the
 * removal of its own screen and reports it through @ref screenLost, but it
 * never destroys or rebuilds itself, so consumers retain full control over
 * the save-state-before-destroy sequencing.
 *
 * `hide()` is synchronous from the caller's perspective. Under the default
 * policy the window is unmapped immediately; under
 * SurfaceConfig::keepMappedOnHide it stays mapped and is made transparent to
 * input instead. We do not expose a transient "Hiding" state because the
 * compositor's acknowledgement is invisible to application code and the
 * surface is unusable until it's either Shown again or recreated.
 */
class PHOSPHORLAYER_EXPORT Surface : public QObject
{
    Q_OBJECT
    Q_PROPERTY(State state READ state NOTIFY stateChanged FINAL)

public:
    enum class State {
        Constructed, ///< Config accepted; no window yet
        Warming, ///< Engine / window / content initialising
        Shown, ///< Layer surface attached; window visible
        Hidden, ///< Attached but window hidden — show() is cheap
        Failed, ///< Unrecoverable (content error, transport rejected, …)
    };
    /// Q_ENUM so QMetaEnum can stringify State values in log messages
    /// (State::Failed → "Failed") and so Q_PROPERTY / QSignalSpy introspection
    /// works. Not currently registered with QML — consumers that want to bind
    /// surface state from QML should register the type themselves via
    /// qmlRegisterUncreatableType<Surface>.
    Q_ENUM(State)

    ~Surface() override;

    State state() const noexcept;
    const SurfaceConfig& config() const noexcept;
    /**
     * @brief Change the surface's keyboard interactivity.
     *
     * On an attached surface it takes effect live, through the transport
     * handle, which commits it. Called before the first attach, it sets the
     * policy the surface attaches with. config().effectiveKeyboard() reports
     * the last value set either way. An out-of-range value, or a call on a
     * Failed surface, warns and is ignored.
     *
     * Two compositor behaviours to design around, both seen under KWin:
     * - Exclusive requested before the first presented frame reaches the
     *   compositor with the first buffer, where focus-stealing prevention may
     *   refuse a new surface the focus. A consumer that needs the grab sets
     *   it after the first frame (ShellHost defers its grab the same way).
     * - Dropping to None does not hand focus back: KWin keeps a surface active
     *   until it is destroyed or another window is activated, so a consumer
     *   that needs focus back hides or destroys the surface.
     */
    void setKeyboardInteractivity(KeyboardInteractivity keyboard);

    /**
     * @brief Convenience: is the Surface in the logical "shown" state?
     *
     * Equivalent to `state() == State::Shown` and exists to give consumers
     * a single canonical answer to "is this overlay currently the
     * user's view of itself" regardless of the underlying lifecycle.
     *
     * Under @ref SurfaceConfig::keepMappedOnHide=true, the QQuickWindow
     * stays Qt-visible across logical hide/show cycles — `window()->isVisible()`
     * is therefore NOT a reliable signal for "currently shown" because it
     * stays `true` while the surface is logically Hidden under
     * `Qt::WindowTransparentForInput`. This helper is the right answer
     * for every lifecycle: keep-mapped surfaces consult Surface state
     * (false during the transparent-for-input span); unmap-on-hide surfaces
     * that stay alive across hides agree with `window()->isVisible()`;
     * destroy-on-hide surfaces are simply gone from the consumer's tracking
     * when hidden, so the call site never reaches isLogicallyShown() on a
     * destroyed Surface.
     */
    [[nodiscard]] bool isLogicallyShown() const noexcept;

    /// @name Lifecycle
    /// Safe to call in any state; invalid transitions warn and no-op.
    /// hide() and warmUp() are no-ops when the surface is already in the
    /// state they ask for. show() on a Shown surface is not: it replays the
    /// root show animation (the root snaps transparent and fades back in)
    /// unless SurfaceConfig::animateRoot is false.
    ///
    /// Declared as Q_SLOTS so consumers can wire QML-side signals (e.g. a
    /// dismissTimer's `dismissRequested()`) directly via the string-based
    /// `QObject::connect(window, SIGNAL(foo()), surface, SLOT(hide()))`
    /// syntax. QML-defined signals can't be addressed via Qt5
    /// `&Class::signal` pointers, so the string form is the only path —
    /// without Q_SLOT registration the connect would silently fail at
    /// runtime.
    /// @{
public Q_SLOTS:
    void show();
    void hide();
    /// Create window + content (and, for contentUrl content, the engine that
    /// loads it), leave hidden. Pre-compiles QML so the first show() is
    /// latency-free.
    void warmUp();
    /// @}

public:
    /// @name Escape hatches
    /// Prefer the declarative API. These are for consumers that need to
    /// reach into Qt machinery (installing event filters, connecting to
    /// window-specific signals, etc.).
    /// @{
    QQuickWindow* window() const noexcept;
    ITransportHandle* transport() const noexcept;
    /// @}

Q_SIGNALS:
    /// NOTIFY signal for the state Q_PROPERTY. Carries the new state so
    /// slots don't need a round-trip getter call. The previous state is
    /// not exposed — track it in your slot if you need it.
    void stateChanged(State newState);

    /// Emitted once when transitioning to State::Failed. `reason` is a
    /// human-readable diagnostic (QML compile error, transport rejection,
    /// etc.). Surface remains in Failed thereafter.
    ///
    /// ## Safe to delete the Surface from this slot
    /// The library defers this signal via `Qt::QueuedConnection` so consumer
    /// slots that call `delete surface` do not re-enter library code on a
    /// dead `this`. The stateChanged(Failed) that precedes it is delivered
    /// from the same deferred call and is delete-safe too (failed() is then
    /// not emitted). For non-failure `stateChanged` emissions the consumer
    /// must prefer `deleteLater()` over `delete`.
    void failed(const QString& reason);

    /// Emitted when the QScreen the Surface is bound to is removed from the
    /// IScreenProvider's list or destroyed, in any state but Failed. The
    /// Surface stays in its current state and config().screen reads nullptr.
    /// An ATTACHED surface (Shown or Hidden) stays bound to the removed output
    /// until the consumer destroys and recreates it; a surface not yet
    /// attached (Constructed or Warming) attaches to the provider's primary
    /// unless the consumer destroys it first. Consumers decide whether to
    /// destroy+rebuild or wait for the screen to come back.
    ///
    /// ## Safe to delete the Surface from this slot
    /// The library defers this signal via `Qt::QueuedConnection` so consumer
    /// slots that call `delete surface` do not re-enter library code on a
    /// dead `this`. Deleting the Surface from a stateChanged slot for
    /// non-Failed transitions remains UB — prefer `deleteLater()` there.
    void screenLost();

public:
    /**
     * @brief Opaque construction token — only SurfaceFactory can create
     * instances of it.
     *
     * Required as the first parameter of Surface's constructor so direct
     * construction (`new Surface(...)`) is impossible without going through
     * `SurfaceFactory::create` / `createAs<T>`, which are the only paths
     * that run the factory-wide validation (role well-formedness, screen
     * fallback, `sharedEngine`+`engineProvider` mutual exclusion).
     *
     * Subclasses wishing to be constructible via `SurfaceFactory::createAs<T>`
     * accept a `CtorToken` as the first argument of their own constructor
     * and forward it to the base. Consumer code cannot instantiate a
     * CtorToken directly (private ctor, only friended to SurfaceFactory),
     * so subclasses cannot bypass the factory even by exposing a public
     * constructor of their own.
     */
    class CtorToken
    {
        friend class SurfaceFactory;
        CtorToken() = default;
    };

protected:
    /// Protected so subclasses can forward a CtorToken through their own
    /// constructor. Still unreachable to consumers — CtorToken is
    /// SurfaceFactory-only-constructible.
    Surface(CtorToken, SurfaceConfig cfg, SurfaceDeps deps, QObject* parent);

private:
    Q_DISABLE_COPY_MOVE(Surface)

    friend class SurfaceFactory;

    class Impl;
    std::unique_ptr<Impl> m_impl;
};

} // namespace PhosphorLayer
