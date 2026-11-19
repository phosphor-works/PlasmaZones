// SPDX-FileCopyrightText: 2026 fuddlesworth
// SPDX-License-Identifier: GPL-3.0-or-later

#pragma once

// Forward declarations (and the few must-be-complete includes threaded
// between them) for daemon.h, split out of the class header. Every entity
// here is consumed by daemon.h as a pointer/reference member or a signature
// type.

#include <PhosphorAnimation/CurveRegistry.h>
#include <PhosphorAnimation/PhosphorProfileRegistry.h>
#include <PhosphorAnimation/QtQuickClockManager.h>
#include <PhosphorConfig/IBackend.h>

namespace PhosphorAnimation {
class CurveLoader;
}

namespace PhosphorAnimationShaders {
class AnimationShaderRegistry;
}

namespace PhosphorShaders {
class ShaderPresetStore;
}

namespace PhosphorSurfaceShaders {
class SurfaceShaderRegistry;
}

namespace PhosphorEngine {
class DesktopRemap;
class WindowRegistry;
}

namespace PhosphorWorkspaces {
class ActivityManager;
class VirtualDesktopManager;
}

// PhosphorRules::RuleSet is held as a value member below
// (m_excludeRuleSet) — needs a complete type, so include the header
// rather than forward-declare. RuleStore stays in the header by
// pointer only; including RuleSet.h leaves the store forward
// declared here.
#include <PhosphorRules/RuleSet.h>

namespace PhosphorRules {
class RuleStore;
class RuleStoreWatcher;
}

namespace PhosphorZones {
class Layout;
class LayoutComputeService;
class LayoutRegistry;
class ScrollingTemplate;
class ScrollingTemplateStore;
class ZoneDetector;
} // namespace PhosphorZones

// `AssignmentEntry::Mode` appears in member-function signatures below, so
// the full struct definition must be visible here (a forward declaration
// can't surface a nested enum). The header is LGPL-LGPL safe (PhosphorZones
// to daemon header is the standard direction).
#include <PhosphorZones/AssignmentEntry.h>

namespace PhosphorEngineRuntime {

enum class DisabledReason;
struct SeedScope;
class Settings;
class OverlayService;

class ShortcutManager;
class LayoutService;
class SettingsService;
class ShaderService;
class ControlService;
class OverlayControlService;
class ZoneQueryService;
class WindowTracker;
class DragResolver;
class IWorkspaceHost;
class ISystemOsd;
class IHostApps;
class IShellCheatsheet;
class RuleService;
class UnifiedLayoutController;
class WorkspaceController;
class TilingService;
class AutotileService;
class ScrollingService;
class ScreenModeRouter;
class CrossSurfaceResolver;
class DaemonScreenModeAdapter;
class DaemonSettingsGateAdapter;
class DaemonWorkspaceStateAdapter;

} // namespace PhosphorEngineRuntime

namespace PhosphorContext {
class ContextResolver;
} // namespace PhosphorContext

namespace PhosphorEngineRuntime {
class SettingsConfigStore;
class SnapService;
class ShaderRegistry;
} // namespace PhosphorEngineRuntime

namespace PhosphorTiles {
class AlgorithmRegistry;
class ScriptedAlgorithmLoader;
}
