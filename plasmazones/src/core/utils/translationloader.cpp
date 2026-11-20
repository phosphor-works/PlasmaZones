// SPDX-FileCopyrightText: 2026 fuddlesworth
// SPDX-License-Identifier: GPL-3.0-or-later

#include "translationloader.h"
#include "common/engineruntimenamespace.h"
#include <PhosphorEngineRuntime/Logging.h>

#include <PhosphorPaths/SuiteDirectories.h>

#include <QCoreApplication>
#include <QDir>
#include <QLibraryInfo>
#include <QLocale>
#include <QTranslator>

#include <memory>

namespace PlasmaZones {

namespace {
/// Install the catalog @p name for the current locale on @p app from the first
/// of @p searchDirs that has one. False when none does.
bool installCatalog(QCoreApplication* app, const QString& name, const QStringList& searchDirs)
{
    // Owned here until it loads, then by the application it is installed on.
    auto translator = std::make_unique<QTranslator>();
    const QLocale locale;
    for (const QString& dir : searchDirs) {
        if (translator->load(locale, name, QStringLiteral("_"), dir)) {
            translator->setParent(app);
            app->installTranslator(translator.release());
            qCDebug(lcCore) << "Loaded" << name << "translations from" << dir << "for locale" << locale.name();
            return true;
        }
    }
    qCDebug(lcCore) << "No" << name << "translations found for locale" << locale.name();
    return false;
}
} // namespace

void loadTranslations(QCoreApplication* app)
{
    Q_ASSERT(app && "loadTranslations() must be called after QCoreApplication is created");
    if (!app)
        return;

    // Search paths in priority order:
    // 1. Alongside the binary: ../share/phosphor-works/plasmazones/translations/ (installed)
    // 2. Build tree root: ../ from bin/ (qt_add_lrelease puts .qm in build root)
    // 3. Build tree: ../translations/ (manual lrelease)
    const QString appDir = QCoreApplication::applicationDirPath();
    installCatalog(
        app, QStringLiteral("plasmazones"),
        {
            appDir + QStringLiteral("/../share/") + PhosphorPaths::appSubdir() + QStringLiteral("/translations"),
            appDir + QStringLiteral("/.."),
            appDir + QStringLiteral("/../translations"),
        });
    // Phosphor.Components' own catalog, for the shared components every
    // PlasmaZones app draws, in the suite's shared translations.
    installCatalog(
        app, QStringLiteral("phosphor-components"),
        {appDir + QStringLiteral("/../share/") + PhosphorPaths::commonDataSubdir(QStringLiteral("translations"))});
}
} // namespace PlasmaZones
