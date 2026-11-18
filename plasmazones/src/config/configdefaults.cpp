// SPDX-FileCopyrightText: 2026 fuddlesworth
// SPDX-License-Identifier: GPL-3.0-or-later

#include "configdefaults.h"

#include "configkeys.h"

#include <PhosphorAnimation/CurveRegistry.h>
#include <PhosphorAnimation/Profile.h>
#include <PhosphorLayoutApi/LayoutId.h>

#include <QDir>
#include <QJsonObject>
#include <QStandardPaths>

#include "core/platform/appdirectoryname.h"

namespace PhosphorEngineRuntime {

static QString configDir()
{
    QString dir = QStandardPaths::writableLocation(QStandardPaths::GenericConfigLocation);
    if (dir.isEmpty()) {
        dir = QDir::homePath() + QStringLiteral("/.config");
    }
    return dir;
}

/// The application's own config directory (appDirectoryName()).
static QString appConfigDir()
{
    return configDir() + QLatin1Char('/') + appDirectoryName();
}

QString ConfigDefaults::configFilePath()
{
    return appConfigDir() + QStringLiteral("/config.json");
}

QString ConfigDefaults::sessionFilePath()
{
    return appConfigDir() + QStringLiteral("/session.json");
}

QString ConfigDefaults::rulesFilePath()
{
    return appConfigDir() + QStringLiteral("/rules.json");
}

QString ConfigDefaults::profilesDir()
{
    return appConfigDir() + QStringLiteral("/profiles");
}

QString ConfigDefaults::quickLayoutsFilePath()
{
    return appConfigDir() + QStringLiteral("/quicklayouts.json");
}

QString ConfigDefaults::layoutSettingsFilePath()
{
    return appConfigDir() + QStringLiteral("/layout-settings.json");
}

QString ConfigDefaults::layoutsDirPath()
{
    return QStandardPaths::writableLocation(QStandardPaths::GenericDataLocation) + QLatin1Char('/')
        + ConfigKeys::layoutsSubdir();
}

QJsonObject ConfigDefaults::defaultLayoutVisibilitySettings()
{
    // Out-of-the-box the picker shows a curated subset; the rest start hidden
    // (users re-show any via the eye toggle). Keys mirror layout-settings.json
    // exactly. The "hiddenFromSelector" key and "autotile:" prefix match
    // PhosphorZones::ZoneJsonKeys::HiddenFromSelector and
    // PhosphorLayout::LayoutId::AutotilePrefix — kept as literals here so the
    // config layer takes no phosphor-zones / phosphor-layout-api dependency.
    const QJsonObject hidden{{QStringLiteral("hiddenFromSelector"), true}};

    // Non-curated standard snapping layouts, by bundled layout UUID. "Wide" is
    // curated (visible by default), so the standard set shows an even eight:
    // BSP, Columns (2), Columns (3), Focus, Grid (2x2), Master + Stack, Rows (2),
    // and Wide.
    static const QStringList layoutIds{
        QStringLiteral("{b8669c74-947a-4551-ba8b-79b6444439e8}"), // Fibonacci
        QStringLiteral("{a40ad8ca-2d60-4418-92cc-01b83420918e}"), // Grid (3x2)
        QStringLiteral("{0c9585bc-ecae-4e87-a6b8-9d34e9b791f2}"), // Priority Grid
        QStringLiteral("{a11899b1-f0e3-4425-9363-acb71726c566}"), // Split Focus
    };

    // Non-curated tiling algorithms (visible set: aligned-grid, bsp, columns,
    // deck, dwindle-memory, master-stack, monocle, three-column). The plain
    // "grid" is hidden by default — "aligned-grid" supersedes it as the curated
    // grid (equal cells, but resize-aware), so only one grid shows out of the box.
    static const QStringList algorithmIds{
        QStringLiteral("cascade"),
        QStringLiteral("centered-master"),
        QStringLiteral("cluster"),
        QStringLiteral("corner-master"),
        QStringLiteral("dwindle"),
        QStringLiteral("floating-center"),
        QStringLiteral("focus-sidebar"),
        QStringLiteral("grid"),
        QStringLiteral("horizontal-deck"),
        QStringLiteral("paper"),
        QStringLiteral("quadrant-priority"),
        QStringLiteral("rows"),
        QStringLiteral("spiral"),
        QStringLiteral("spread"),
        QStringLiteral("stair"),
        QStringLiteral("tatami"),
        QStringLiteral("theater"),
        QStringLiteral("wide"),
        QStringLiteral("zen"),
    };

    QJsonObject out;
    for (const QString& id : layoutIds) {
        out.insert(id, hidden);
    }
    for (const QString& id : algorithmIds) {
        out.insert(PhosphorLayout::LayoutId::makeAutotileId(id), hidden);
    }
    return out;
}

QString ConfigDefaults::legacyConfigFilePath()
{
    // The single-file INI the v1 config lived in, beside the directory.
    return configDir() + QLatin1Char('/') + appDirectoryName() + QStringLiteral("rc");
}

PhosphorAnimation::CurveRegistry& ConfigDefaults::fallbackCurveRegistry()
{
    static PhosphorAnimation::CurveRegistry registry;
    return registry;
}

QVariantMap ConfigDefaults::animationProfile(const PhosphorAnimation::CurveRegistry& registry)
{
    // Curve is resolved through the CurveRegistry so the wire format
    // ("0.33,1.00,0.68,1.00") maps to the same shared_ptr<const Curve>
    // the runtime uses.
    PhosphorAnimation::Profile p;
    p.curve = registry.create(animationEasingCurve());
    p.duration = static_cast<qreal>(animationDuration());
    p.minDistance = animationMinDistance();
    p.sequenceMode = static_cast<PhosphorAnimation::SequenceMode>(animationSequenceMode());
    p.staggerInterval = animationStaggerInterval();
    return p.toJson().toVariantMap();
}

} // namespace PhosphorEngineRuntime
