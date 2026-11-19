// SPDX-FileCopyrightText: 2026 fuddlesworth
// SPDX-License-Identifier: GPL-3.0-or-later

#include "controlservice.h"
#include "runtime/services/snapservice/snapservice.h"
#include "runtime/windowtracker/windowtracker.h"
#include <PhosphorZones/LayoutRegistry.h>
#include <PhosphorZones/Layout.h>
#include <PhosphorZones/Zone.h>
#include <PhosphorEngineRuntime/Logging.h>
#include <PhosphorEngine/IPlacementEngine.h>

#include <QJsonDocument>
#include <QJsonObject>
#include <QJsonArray>

#include <memory>

namespace PhosphorEngineRuntime {

ControlService::ControlService(WindowTracker* wta, SnapService* snapService,
                               PhosphorZones::LayoutRegistry* layoutManager,
                               PhosphorEngine::IPlacementEngine* autotileEngine, QObject* parent)
    : QObject(parent)
    , m_wta(wta)
    , m_snapService(snapService)
    , m_layoutManager(layoutManager)
    , m_autotileEngine(autotileEngine)
{
}

void ControlService::snapWindowToZone(const QString& windowId, int zoneNumber, const QString& screenId)
{
    // Any zone the layout has (F85); a missing number is reported as not found.
    if (windowId.isEmpty() || zoneNumber < 1) {
        qCWarning(lcDbusWindow) << "snapWindowToZone: invalid args windowId=" << windowId
                                << "zoneNumber=" << zoneNumber;
        return;
    }
    // The zone is resolved in the layout of the screen the call names, a
    // screen id or a connector name (a split monitor's connector resolves to
    // the virtual screen the window is in), or of the window's own screen when
    // it names none (F22, F26), and snapped there through the bus gate.
    if (m_snapService) {
        m_snapService->moveWindowToZoneNumberOnScreen(windowId, zoneNumber, screenId);
    }
}

void ControlService::toggleAutotileForScreen(const QString& screenId)
{
    if (screenId.isEmpty()) {
        qCWarning(lcDbusWindow) << "toggleAutotileForScreen: empty screenId";
        return;
    }
    // A connector name or a split monitor's id resolves to the virtual screen
    // in question (F22), and the switch itself is the mode-toggle shortcut's
    // own code on that screen: its context, its remembered layout or
    // algorithm, its feature gate and disabled-context notice (L13 Q1).
    const QString resolved = m_wta ? m_wta->resolveBusScreen(screenId) : QString();
    if (resolved.isEmpty()) {
        qCWarning(lcDbusWindow) << "toggleAutotileForScreen: unknown screen" << screenId;
        return;
    }
    if (!m_modeToggle) {
        qCWarning(lcDbusWindow) << "toggleAutotileForScreen: no mode toggle wired";
        return;
    }
    qCInfo(lcDbusWindow) << "toggleAutotileForScreen:" << screenId << "->" << resolved;
    m_modeToggle(resolved);
}

void ControlService::setModeToggleHandler(std::function<void(const QString& screenId)> handler)
{
    m_modeToggle = std::move(handler);
}

QString ControlService::getFullState()
{
    QJsonObject state;

    // Layouts
    if (m_layoutManager) {
        QJsonArray layoutArray;
        for (PhosphorZones::Layout* layout : m_layoutManager->layouts()) {
            QJsonObject lo;
            lo[QLatin1String("id")] = layout->id().toString();
            lo[QLatin1String("name")] = layout->name();
            lo[QLatin1String("zoneCount")] = layout->zoneCount();
            layoutArray.append(lo);
        }
        state[QLatin1String("layouts")] = layoutArray;

        PhosphorZones::Layout* active = m_layoutManager->activeLayout();
        if (active) {
            state[QLatin1String("activeLayoutId")] = active->id().toString();
            state[QLatin1String("activeLayoutName")] = active->name();
        }
    }

    // Window states (via WTA)
    if (m_wta) {
        QJsonArray windowsArray;
        for (const auto& ws : m_wta->getAllWindowStates()) {
            QJsonObject wsObj;
            wsObj[QLatin1String("windowId")] = ws.windowId;
            wsObj[QLatin1String("zoneId")] = ws.zoneId;
            wsObj[QLatin1String("screenId")] = ws.screenId;
            wsObj[QLatin1String("isFloating")] = ws.isFloating;
            windowsArray.append(wsObj);
        }
        state[QLatin1String("windows")] = windowsArray;
    }

    // Autotile state
    if (m_autotileEngine) {
        QJsonObject autotile;
        autotile[QLatin1String("enabled")] = m_autotileEngine->isEnabled();
        QJsonArray screens;
        for (const QString& s : m_autotileEngine->activeScreens()) {
            screens.append(s);
        }
        autotile[QLatin1String("screens")] = screens;
        state[QLatin1String("autotile")] = autotile;
    }

    return QString::fromUtf8(QJsonDocument(state).toJson(QJsonDocument::Compact));
}

void ControlService::detach()
{
    m_wta = nullptr;
    m_snapService = nullptr;
    m_layoutManager = nullptr;
    m_autotileEngine = nullptr;
    m_shortcutCatalog = nullptr;
    m_modeToggle = nullptr;
}

} // namespace PhosphorEngineRuntime
