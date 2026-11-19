// SPDX-FileCopyrightText: 2026 fuddlesworth
// SPDX-License-Identifier: GPL-3.0-or-later

#include "runtime/engine/engineruntime.h"
#include "helpers.h"

#include <PhosphorAnimation/PhosphorProfileRegistry.h>

#include "runtime/overlay/overlayservice.h"
#include "runtime/controllers/shortcutmanager.h"

#include <PhosphorIdentity/WindowId.h>
#include <PhosphorZones/LayoutRegistry.h>
#include <PhosphorZones/IZoneLayoutRegistry.h>
#include <PhosphorZones/ZoneDetector.h>
#include <PhosphorTiles/AlgorithmRegistry.h>
#include <PhosphorTiles/ITileAlgorithmRegistry.h>
#include <PhosphorTiles/ScriptedAlgorithmLoader.h>
#include <PhosphorEngine/WindowRegistry.h>
#include <PhosphorWorkspaces/VirtualDesktopManager.h>
#include <PhosphorWorkspaces/ActivityManager.h>
#include <PhosphorRules/RuleStore.h>

#include <PhosphorEngineRuntime/SettingsConfigStore.h>
#include <PhosphorEngineRuntime/Settings.h>
#include <PhosphorEngineRuntime/Constants.h>
#include <PhosphorEngineRuntime/Logging.h>
#include <PhosphorEngineRuntime/ShaderRegistry.h>
#include "runtime/services/layoutservice/layoutservice.h"
#include "runtime/services/settingsservice/settingsservice.h"
#include "runtime/services/overlaycontrolservice.h"
#include "runtime/services/zonequeryservice.h"
#include "runtime/windowtracker/windowtracker.h"
#include "runtime/dragresolver/dragresolver.h"
#include "runtime/services/tilingservice/tilingservice.h"
// Needed as COMPLETE types, not just forward declarations: the re-entry
// preamble below deletes them, and deleting through an incomplete type calls
// neither the destructor nor a class-specific operator delete.
#include "runtime/services/autotileservice/autotileservice.h"
#include "runtime/services/scrollingservice/scrollingservice.h"
#include "runtime/services/snapservice/snapservice.h"
#include "runtime/services/shaderservice.h"
#include "runtime/services/controlservice.h"
#include "runtime/services/ruleservice.h"

namespace PhosphorEngineRuntime {

void EngineRuntime::initCoreAdaptors()
{
    // stop() -> init() re-entry: every adaptor below survives stop() (it only
    // detaches / clears their borrows), so construct-over would leak one full
    // adaptor set as QObject children per cycle, each still holding its
    // init-time connections. Delete the whole previous set first, borrowers
    // before borrowed: ControlService (borrows snap/layout/WTA), then the
    // engine services (borrow the WTA), then the core services in reverse
    // creation order. The host has already dropped its bindings to them.
    //
    // Manual delete of Qt-parented QObjects is a deliberate carve-out from
    // the never-manual-delete rule: init() runs before exec(), so no queued
    // dispatch or event can land mid-delete, and deleteLater() would NOT be
    // processed before the host binds the new set.
    delete m_controlService;
    m_controlService = nullptr;
    delete m_snapService;
    m_snapService = nullptr;
    delete m_autotileService;
    m_autotileService = nullptr;
    // TilingService and ScrollingService belong to this set too: both are
    // unconditionally re-newed in initEnginesAndWiring, which documents these
    // members as null on a re-cycle. Omitting them leaked the old pair, and
    // the host would have bound a second Tiling/Scrolling surface beside the
    // first after a daemon restart.
    delete m_tilingService;
    m_tilingService = nullptr;
    delete m_scrollingService;
    m_scrollingService = nullptr;
    delete m_dragResolver;
    m_dragResolver = nullptr;
    delete m_windowTracker;
    m_windowTracker = nullptr;
    delete m_zoneQueryService;
    m_zoneQueryService = nullptr;
    delete m_overlayControlService;
    m_overlayControlService = nullptr;
    delete m_ruleService;
    m_ruleService = nullptr;
    delete m_shaderService;
    m_shaderService = nullptr;
    delete m_settingsService;
    m_settingsService = nullptr;
    delete m_layoutService;
    m_layoutService = nullptr;

    // The services behind the public interfaces. Raw new; Qt parent-child
    // manages their lifetime. The host binds each to its transport after init().
    m_layoutService =
        new LayoutService(m_layoutManager.get(), m_virtualDesktopManager.get(), m_screenManager.get(), this);
    m_layoutService->setActivityManager(m_activityManager.get());
    m_layoutService->setSettings(m_settings.get());
    m_layoutService->setHostApps(m_hostApps);
    m_layoutService->setLayoutsAvailableResolver([this](const QString& screenId) {
        return !m_shuttingDown && anyModeEnabled()
            && layoutSupportForScreen(screenId) != PhosphorEngine::IPlacementEngine::LayoutSupport::None;
    });
    m_layoutService->setAlgorithmRegistry(m_algorithmRegistry.get());
    m_layoutService->setLayoutSource(m_layoutSources.composite());
    // Thread the bundle-owned autotile source through the adaptor's
    // buildUnifiedLayoutList path so its preview cache survives across
    // D-Bus calls. The full composite above drives the
    // getLayoutPreview* methods; this separate pointer targets only the
    // autotile enumeration slot — see LayoutService::setAutotileLayoutSource.
    m_layoutService->setAutotileLayoutSource(m_autotileLayoutSource);
    // The live tiled-window count getScreenStates publishes, the same answer
    // the registry's TiledWindowCount provider gives the context cascade. The
    // engines are built later in init(); the lambda reads them per call.
    m_layoutService->setTiledWindowCountProvider([this](const QString& screenId) {
        return tiledWindowCountForScreen(screenId);
    });
    // Invalidate D-Bus getActiveLayout() cache when the default layout changes in settings
    connect(m_settings.get(), &Settings::defaultLayoutIdChanged, m_layoutService, &LayoutService::invalidateCache);
    m_settingsService = new SettingsService(m_settings.get(), &m_profileRegistry, this);

    // Shader service - shader discovery, compilation lifecycle, file monitoring.
    // Held as a member so stop() can detach() it before
    // the unique_ptr member that owns m_shaderRegistry runs its destructor.
    m_shaderService = new ShaderService(m_shaderRegistry.get(), this);

    // Rule service - the unified org.phosphor_works.Placement.Rules surface.
    // Held as a member so stop() can detach() it before
    // the unique_ptr that owns m_ruleStore runs its destructor.
    m_ruleService = new RuleService(m_ruleStore.get(), this);

    // Session-idle detection for Decorations.Performance.PauseWhenIdle. The KWin
    // effect cannot do this itself: idleness arrives over ext-idle-notify-v1, a
    // Wayland CLIENT protocol that the compositor the effect lives in SERVES rather
    // than consumes. The daemon is already a client, so it watches and pushes the
    // resolved boolean over D-Bus.
    //
    // Worth the plumbing because it is the only thing that recovers idle GPU power:
    // an animated decoration pack repaints every window carrying it on every vsync,
    // and it is the existence of per-frame work — not how cheap that work is — that
    // keeps the card pinned in its top performance state.
    //
    // Constructed AFTER m_settingsService (it emits through it). The host's
    // compositorConnected() pushes the current idle state to a freshly
    // (re)started bridge, since the signal is edge-triggered.
    setupIdleService();

    // Overlay control - overlay visibility and highlighting
    m_overlayControlService =
        new OverlayControlService(m_overlayService.get(), m_zoneDetector.get(), m_layoutManager.get(),
                                  m_screenManager.get(), m_settings.get(), this);

    // Zone detection queries
    m_zoneQueryService = new ZoneQueryService(m_zoneDetector.get(), m_layoutManager.get(), m_screenManager.get(),
                                              m_settings.get(), this);

    // Window tracking - window-zone assignments
    m_windowTracker = new WindowTracker(m_layoutManager.get(), m_zoneDetector.get(), m_screenManager.get(),
                                        m_settings.get(), m_virtualDesktopManager.get(), m_activityManager.get(), this);
    m_windowTracker->setZoneAdjacencyResolver(m_zoneQueryService);
    m_windowTracker->setWindowRegistry(m_windowRegistry.get());
    // Full rule set for per-window RestorePosition evaluation (overrides the
    // per-engine *RestoreFloatedWindowsOnLogin settings for matched windows).
    m_windowTracker->setRuleStore(m_ruleStore.get());
    // The effect's settle report can name an output gone before the daemon's
    // own screen-removed handler runs; both retire it the same way.
    m_windowTracker->setOutputRetirer([this](const QString& physicalScreenId) {
        retireOutputPlacements(physicalScreenId);
    });
    // A window that left a screen leaves the saved engine orders of the
    // screens it left, or a mode round trip there tiles it back (F695).
    m_windowTracker->setWindowLeftScreenHook([this](const QString& windowId, const QString& keepScreenId) {
        pruneEngineOrdersForWindow(PhosphorIdentity::WindowId::extractInstanceId(windowId), keepScreenId);
    });
    // An evacuee on a hidden desktop floats by the mode that desktop runs,
    // decided as the engine screen sets decide it in view (F1122).
    m_windowTracker->setContextModeResolver([this](const QString& screenId, int desktop, const QString& activity) {
        return runningModeForContext(screenId, desktop, activity);
    });
    // Each monitor's size as last accepted from the compositor, from the
    // session state the adaptor just loaded, stands in for the report until
    // this session's is accepted (onCompositorPixelSizeChanged). Here, before start()
    // starts the screen manager, so the first resolve of every screen already
    // has it and a report that confirms it moves nothing. The manager tracks
    // no screen yet, so seeding emits nothing.
    {
        const QHash<QString, QSize> lastSizes = m_windowTracker->screenPixelSizes();
        for (auto it = lastSizes.constBegin(); it != lastSizes.constEnd(); ++it) {
            m_screenManager->seedPixelSize(it.key(), it.value());
        }
    }

    // Drop closed windows from m_lastEngineOrders so a manual→autotile toggle
    // doesn't replay a ghost id into the TilingState (recalculateLayout would
    // then tile N+1 windows for N actual windows). The registry is ctor-owned
    // and survives stop(), so sever this daemon-receiver connection first: a
    // stop() → init() cycle would otherwise stack a second prune handler.
    disconnect(m_windowRegistry.get(), &PhosphorEngine::WindowRegistry::windowDisappeared, this, nullptr);
    connect(m_windowRegistry.get(), &PhosphorEngine::WindowRegistry::windowDisappeared, this,
            [this](const QString& instanceId) {
                pruneEngineOrdersForWindow(instanceId);
            });

    // Reapply window geometries after each geometry batch (processPendingGeometryUpdates).
    // When the delayed panel requery completes it emits availableGeometryChanged, which triggers
    // the same debounce → processPendingGeometryUpdates → reapply path; no separate delay needed.
    m_reapplyGeometriesTimer.setSingleShot(true);
    connect(&m_reapplyGeometriesTimer, &QTimer::timeout, m_windowTracker,
            &WindowTracker::requestReapplyWindowGeometries);

    // Window drag - handles drag events from the compositor.
    // All drag logic (modifiers, zones, snapping) handled in the resolver.
    m_dragResolver = new DragResolver(m_overlayService.get(), m_zoneDetector.get(), m_layoutManager.get(),
                                      m_screenManager.get(), m_settings.get(), m_windowTracker, this);

    // PhosphorZones::Zone selector methods are called directly from DragResolver.

    // Give the drag resolver access to the shortcut manager for
    // registering/unregistering the Escape cancel shortcut during drags.
    // Routed through the PhosphorShortcutsIntegration::IAdhocRegistrar interface so the underlying
    // Registry stays private to ShortcutManager.
    m_dragResolver->setShortcutRegistrar(m_shortcutManager.get());

    // Initialize scripted algorithm loader BEFORE engine construction so that
    // user-defined algorithms are registered in the daemon registry before
    // the engine resolves the configured algorithm ID.
    m_scriptedAlgorithmLoader =
        std::make_unique<PhosphorTiles::ScriptedAlgorithmLoader>(scriptedAlgorithmSubdir(), m_algorithmRegistry.get());
    // When scripted algorithms change (hot-reload), notify layout list consumers
    connect(m_scriptedAlgorithmLoader.get(), &PhosphorTiles::ScriptedAlgorithmLoader::algorithmsChanged, this,
            [this]() {
                if (m_layoutService)
                    m_layoutService->notifyLayoutListChanged();
            });
    m_scriptedAlgorithmLoader->scanAndRegister();
}

} // namespace PhosphorEngineRuntime
