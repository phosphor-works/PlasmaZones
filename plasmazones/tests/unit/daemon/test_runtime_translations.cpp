// SPDX-FileCopyrightText: 2026 fuddlesworth
// SPDX-License-Identifier: GPL-3.0-or-later

// The engine runtime translates through its own catalog: its C++ strings and
// the shortcut names marked with QT_TRANSLATE_NOOP all resolve under the
// PhosphorEngineRuntime context in phosphor-engine-runtime_<lang>.qm. A marker
// or a tr() call left on another context compiles and simply stops
// translating, so this loads the built German catalog and asks.

#include "runtime/common/runtimei18n.h"

#include <QCoreApplication>
#include <QTest>
#include <QTranslator>

class TestRuntimeTranslations : public QObject
{
    Q_OBJECT

private Q_SLOTS:
    void runtimeStringsResolveInTheRuntimeCatalog()
    {
        QTranslator translator;
        QVERIFY2(translator.load(QStringLiteral(RUNTIME_DE_QM)), RUNTIME_DE_QM);
        QCoreApplication::installTranslator(&translator);

        // A RuntimeI18n::tr() string.
        QCOMPARE(RuntimeI18n::tr("Cancel Zone Overlay"), QStringLiteral("Zonen-Overlay abbrechen"));
        // A shortcut name marked QT_TRANSLATE_NOOP("PhosphorEngineRuntime", ...)
        // in shortcutmanager_table.cpp and translated by RuntimeI18n::tr().
        QCOMPARE(RuntimeI18n::tr("Open Settings"), QStringLiteral("Einstellungen öffnen"));

        QCoreApplication::removeTranslator(&translator);
    }
};

QTEST_GUILESS_MAIN(TestRuntimeTranslations)
#include "test_runtime_translations.moc"
