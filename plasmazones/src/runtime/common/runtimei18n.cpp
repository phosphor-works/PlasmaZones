// SPDX-FileCopyrightText: 2026 fuddlesworth
// SPDX-License-Identifier: GPL-3.0-or-later

#include "runtime/common/runtimei18n.h"

#include "core/platform/logging.h"

#include <QLocale>
#include <QTranslator>

#include <memory>

namespace PhosphorEngineRuntime {

void loadRuntimeTranslations(QCoreApplication* app)
{
    Q_ASSERT(app && "loadRuntimeTranslations() must be called after QCoreApplication is created");
    if (!app) {
        return;
    }
    // Owned here until it loads, then by the application it is installed on.
    auto translator = std::make_unique<QTranslator>();
    const QLocale locale;
    // Installed beside the binary's share/, then the build tree root, where
    // qt_add_lrelease writes the .qm files.
    const QString appDir = QCoreApplication::applicationDirPath();
    const QStringList searchDirs = {
        appDir + QStringLiteral("/../share/phosphor-engine-runtime/translations"),
        appDir + QStringLiteral("/.."),
    };
    for (const QString& dir : searchDirs) {
        if (translator->load(locale, QStringLiteral("phosphor-engine-runtime"), QStringLiteral("_"), dir)) {
            translator->setParent(app);
            app->installTranslator(translator.release());
            qCDebug(lcCore) << "Loaded engine runtime translations from" << dir << "for locale" << locale.name();
            return;
        }
    }
    qCDebug(lcCore) << "No engine runtime translations found for locale" << locale.name();
}

} // namespace PhosphorEngineRuntime
