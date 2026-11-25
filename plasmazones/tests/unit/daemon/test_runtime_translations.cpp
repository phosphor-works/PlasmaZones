// SPDX-FileCopyrightText: 2026 Phosphor Works Team
// SPDX-License-Identifier: GPL-3.0-or-later

// The engine runtime translates through its own catalog: its C++ strings and
// the shortcut names marked with QT_TRANSLATE_NOOP resolve under the
// PhosphorEngineRuntime context, and the overlay QML's qsTr() under each
// file's name, all in phosphor-engine-runtime_<lang>.qm. A marker
// or a tr() call left on another context compiles and simply stops
// translating, so this loads the built German catalog and asks.

#include <PhosphorEngineRuntime/RuntimeI18n.h>

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
        // core/'s strings, in the runtime's context like its own: the layout
        // list's section labels and its disambiguated "None" rows.
        QCOMPARE(RuntimeI18n::tr("All Monitors"), QStringLiteral("Alle Bildschirme"));
        QCOMPARE(RuntimeI18n::tr("None", "the explicit no-layout choice"), QStringLiteral("Keines"));
        // The overlay QML's qsTr(), under its file name.
        QCOMPARE(QCoreApplication::translate("ZoneSelectorContent", "No columns yet. Drop here to start the strip.",
                                             "@info strip selector empty state"),
                 QStringLiteral("Noch keine Spalten. Hier ablegen, um den Streifen zu beginnen."));

        QCoreApplication::removeTranslator(&translator);
    }

    // The English catalog is compiled too, for the plural forms of the overlay
    // QML's qsTr() calls that pass a count.
    void englishCarriesTheOverlayPlurals()
    {
        QTranslator translator;
        QVERIFY2(translator.load(QStringLiteral(RUNTIME_EN_QM)), RUNTIME_EN_QM);
        QCoreApplication::installTranslator(&translator);
        QCOMPARE(QCoreApplication::translate("NavigationOsdContent", "Rotated %n window", nullptr, 3),
                 QStringLiteral("Rotated 3 windows"));
        QCOMPARE(QCoreApplication::translate("NavigationOsdContent", "Rotated %n window", nullptr, 1),
                 QStringLiteral("Rotated 1 window"));
        QCoreApplication::removeTranslator(&translator);
    }
};

QTEST_GUILESS_MAIN(TestRuntimeTranslations)
#include "test_runtime_translations.moc"
