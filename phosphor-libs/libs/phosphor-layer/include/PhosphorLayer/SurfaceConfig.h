// SPDX-FileCopyrightText: 2026 fuddlesworth
// SPDX-License-Identifier: LGPL-2.1-or-later

#pragma once

#include <PhosphorLayer/Role.h>
#include <PhosphorLayer/phosphorlayer_export.h>

#include <QMargins>
#include <QSize>
#include <QString>
#include <QUrl>
#include <QVariantMap>

#include <memory>
#include <optional>

QT_BEGIN_NAMESPACE
class QQmlEngine;
class QQuickItem;
class QScreen;
QT_END_NAMESPACE

namespace PhosphorLayer {

/**
 * @brief Immutable per-surface configuration passed to SurfaceFactory::create().
 *
 * Aggregates a Role (protocol-level defaults) with per-instance data: content
 * source, target screen, per-instance overrides of role defaults, optional
 * shared QML engine.
 *
 * Reconfiguration = destroy + recreate. This matches wlr-layer-shell, which
 * forbids most post-show property changes (layer, anchors, output, scope
 * are all immutable per protocol). Keyboard interactivity is the exception
 * the protocol allows: Surface::setKeyboardInteractivity changes it live.
 *
 * Exactly one of @ref contentUrl / @ref contentItem must be non-empty.
 *
 * @note **Pre-1.0 ABI.** SurfaceConfig is a plain aggregate exposed across
 * the DSO boundary. Adding or reordering fields between releases is a
 * binary-incompatible change until the library reaches 1.0 (SOVERSION 0
 * signals this). Consumers that aggregate-init by position must rebuild
 * against each release; use named-member init for forward compatibility.
 */
struct PHOSPHORLAYER_EXPORT SurfaceConfig
{
    /// Protocol-level defaults. Required.
    Role role;

    /// QML root component URL (`qrc:/…` or `file://…`). Mutually exclusive
    /// with @ref contentItem.
    QUrl contentUrl;

    /// Pre-built QQuickItem. Ownership is transferred to the Surface at
    /// construction. Mutually exclusive with @ref contentUrl.
    /// Note: unique_ptr is intentionally non-copyable — SurfaceConfig
    /// containing a contentItem is move-only.
    std::unique_ptr<QQuickItem> contentItem;

    /// Target screen. Nullptr means "let the factory resolve to the screen
    /// provider's primary()". Consumers that want focus-follows-surface can
    /// pass `IScreenProvider::focused()` here.
    QScreen* screen = nullptr;

    /// Injected into the surface's QML root context. Keys become context
    /// properties visible to the loaded QML as global identifiers. For
    /// @ref contentUrl content only: a @ref contentItem was created in its
    /// caller's own context, and the factory refuses the combination rather
    /// than ignore the properties.
    QVariantMap contextProperties;

    /// Applied to the QQuickWindow before the QML content completes. For
    /// @ref contentItem or Item-rooted QML that is the wrapper window, and
    /// every key goes through QObject::setProperty when that window is
    /// created, before the content is parented into it. For Window-rooted
    /// QML it is the QML root itself, between beginCreate and completeCreate:
    /// a key the root declares (its own QML `property` or a QQuickWindow
    /// property) is set as an initial property, anything else as a dynamic
    /// one via QObject::setProperty. Only declared properties are visible to
    /// QML bindings; a dynamic one is for C++ readers (QObject::property). A
    /// dotted key naming a nested property is not supported and is skipped
    /// with a warning. Distinct from @ref contextProperties: window
    /// properties are per-surface (won't leak between shared-engine
    /// surfaces) and are observable through Qt's property system.
    QVariantMap windowProperties;

    /// Opt-in shared QQmlEngine for @ref contentUrl content. Nullptr
    /// (default) → the Surface takes one from the engine provider, or owns
    /// its own for full isolation. Non-null → Surface uses the provided
    /// engine; caller retains ownership, and the engine must outlive the
    /// Surface's teardown, which defers deleting the window (and any
    /// per-surface child context) to the event loop. A @ref contentItem
    /// surface needs no engine and ignores it.
    QQmlEngine* sharedEngine = nullptr;

    // ── Per-instance overrides ──────────────────────────────────────────
    // nullopt = use the role's default. All are applied at surface-creation
    // time; post-show mutation of these fields has no effect (the Surface
    // holds its own copy privately and exposes it only through
    // Surface::config() const). keyboardOverride is the one exception:
    // Surface::setKeyboardInteractivity updates the Surface's copy and the
    // live transport handle.

    std::optional<Layer> layerOverride;
    std::optional<Anchors> anchorsOverride;
    std::optional<int> exclusiveZoneOverride;
    std::optional<KeyboardInteractivity> keyboardOverride;
    std::optional<QMargins> marginsOverride;

    // ── Lifecycle policy ────────────────────────────────────────────────
    /// When true, Surface::hide() does NOT call QQuickWindow::hide(); the
    /// surface stays Qt-visible and the visual "hidden" state is driven by
    /// the injected ISurfaceAnimator (typically by animating the root QML
    /// item's opacity to 0). The library still flips the underlying
    /// QWindow's `Qt::WindowTransparentForInput` flag during the hide so
    /// the still-mapped layer surface stops intercepting clicks at its
    /// screen position.
    ///
    /// Use this for overlays where reattaching the wl_surface (and its
    /// Vulkan swapchain) on every show/hide cycle is too expensive. The
    /// cost of staying mapped is that the surface is composited for as
    /// long as it lives, and a transparent surface above a fullscreen
    /// window stops the compositor from scanning that window out directly.
    /// No in-tree consumer sets it today.
    ///
    /// With animateRoot=false nothing animates the root, so hide() only
    /// flips input transparency and the consumer owns the hidden visual.
    ///
    /// Input transparency covers pointer and touch, not the keyboard: a
    /// hidden surface that stays mapped keeps whatever keyboard interactivity
    /// it had, Exclusive included. A consumer drops it with
    /// Surface::setKeyboardInteractivity(None) on hide and restores it on
    /// show, and under KWin even None does not hand focus back until another
    /// window is activated.
    ///
    /// Default `false` preserves the original lifecycle: hide() unmaps the
    /// window immediately and a future show() pays the reattach cost.
    bool keepMappedOnHide = false;

    /// Initial size committed to the wl_surface during warm-up. Determines
    /// the size of the first Vulkan swapchain Qt allocates.
    ///
    /// When @c isEmpty() (the default `QSize{}`), warm-up sizes the wrapper
    /// QQuickWindow to the target screen's full geometry. That guarantees a
    /// non-zero buffer for partial-anchor layer surfaces (see the "non-zero
    /// size BEFORE completeCreate" invariant in
    /// surface.cpp::instantiateFromComponent), but it costs a full-screen
    /// swapchain (~25 MB at 4K × 3 buffers on NVIDIA) even when the eventual
    /// visible content is a small toast.
    ///
    /// When non-empty, surface.cpp uses this as the warm-up geometry instead.
    /// Subsequent imperative setWidth/setHeight calls still resize the
    /// surface; this only governs the size of the warm-up commit. Pass the
    /// largest size the surface is expected to grow to in practice — that
    /// way Qt avoids the "small swapchain → resize on first show" round
    /// trip on NVIDIA, where swapchain resize is destroy + recreate.
    ///
    /// `QSize::isEmpty()` (any non-positive dimension) is the unset
    /// sentinel; partially-zero sizes are silently treated as "use the
    /// default" — pass a fully-positive size or leave it default.
    QSize initialSize = {};

    /// Logged in state transitions. Defaults to Role::scopePrefix when empty.
    QString debugName;

    /// When false, the surface's own show/hide never dispatches
    /// ISurfaceAnimator::beginShow / beginHide on its root item, so the
    /// root's opacity and scale are left alone. For a surface that hosts
    /// several independently animated items (a shell with per-content
    /// slots) and drives each one through the animator itself, where a
    /// root-level fade would multiply every item's own transition.
    ///
    /// ISurfaceAnimator::cancel still retires every animation keyed on this
    /// surface, the consumer's per-item ones included, at destruction and
    /// on the Hidden → Shown and Shown → Hidden edges. A redundant show()
    /// on a Shown surface does not cancel. The edge cancels drop an
    /// in-flight item leg without running its completion, so a consumer
    /// must call show() BEFORE starting item legs and hide() only after
    /// they have settled. The keepMappedOnHide=false hide warning is
    /// skipped too, since there is no root fade for the unmap to cut short.
    ///
    /// Default `true` keeps the root-animated lifecycle.
    bool animateRoot = true;

    // ── Resolution helpers ──────────────────────────────────────────────

    [[nodiscard]] Layer effectiveLayer() const noexcept
    {
        return layerOverride.value_or(role.layer);
    }
    [[nodiscard]] Anchors effectiveAnchors() const noexcept
    {
        return anchorsOverride.value_or(role.anchors);
    }
    [[nodiscard]] int effectiveExclusiveZone() const noexcept
    {
        return exclusiveZoneOverride.value_or(role.exclusiveZone);
    }
    [[nodiscard]] KeyboardInteractivity effectiveKeyboard() const noexcept
    {
        return keyboardOverride.value_or(role.keyboard);
    }
    [[nodiscard]] QMargins effectiveMargins() const noexcept
    {
        return marginsOverride.value_or(role.defaultMargins);
    }
    [[nodiscard]] QString effectiveDebugName() const
    {
        return debugName.isEmpty() ? role.scopePrefix : debugName;
    }

    // Special members defined out-of-line (in surface.cpp) so QQuickItem
    // need only be forward-declared here — the concrete ~unique_ptr<QQuickItem>
    // call sites live where QQuickItem is complete.
    SurfaceConfig();
    ~SurfaceConfig();
    SurfaceConfig(const SurfaceConfig&) = delete;
    SurfaceConfig& operator=(const SurfaceConfig&) = delete;
    SurfaceConfig(SurfaceConfig&&) noexcept;
    SurfaceConfig& operator=(SurfaceConfig&&) noexcept;
};

} // namespace PhosphorLayer
