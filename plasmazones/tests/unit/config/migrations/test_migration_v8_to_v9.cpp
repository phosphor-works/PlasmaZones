// SPDX-FileCopyrightText: 2026 fuddlesworth
// SPDX-License-Identifier: GPL-3.0-or-later

/**
 * @file test_migration_v8_to_v9.cpp
 * @brief Unit tests for the v8 → v9 schema migration (config → config).
 *
 * v9 makes every structured key native. The settings store reads a key the
 * schema types as a JSON object or array from the native node only, so a
 * JSON-ENCODED STRING in such a key (migrateV1ToV2's Animations/Profile, or a
 * trigger list stored by a build from before the store wrote structured values
 * natively) has to become the object or array it spells. The migration must:
 *   - replace a string that parses to the right shape with the native value,
 *     for top-level and dot-path groups alike;
 *   - leave alone a string that does not parse, or parses to the wrong shape;
 *   - leave a native value, a non-structured key and an absent key untouched;
 *   - stamp the literal 9, and do nothing to a root already at v9;
 *   - leave the full chain from a v1 INI ending with native values the
 *     settings model reads.
 */

#include <QDir>
#include "common/engineruntimenamespace.h"
#include <QFile>
#include <QFileInfo>
#include <QJsonArray>
#include <QJsonDocument>
#include <QJsonObject>
#include <QTest>
#include <QTextStream>

#include <PhosphorEngineRuntime/ConfigDefaults.h>
#include <PhosphorEngineRuntime/ConfigMigration.h>
#include <PhosphorEngineRuntime/Settings.h>
#include "helpers/IsolatedConfigGuard.h"

using namespace PlasmaZones;
using PlasmaZones::TestHelpers::IsolatedConfigGuard;

namespace {
const QString kVersion = QStringLiteral("_version");

QString compact(const QJsonObject& obj)
{
    return QString::fromUtf8(QJsonDocument(obj).toJson(QJsonDocument::Compact));
}

QString compact(const QJsonArray& arr)
{
    return QString::fromUtf8(QJsonDocument(arr).toJson(QJsonDocument::Compact));
}

QJsonObject trigger(int modifier)
{
    return QJsonObject{{QStringLiteral("modifier"), modifier}, {QStringLiteral("mouseButton"), 0}};
}

/// The object at a dot-path group of a nested root.
QJsonObject groupAt(const QJsonObject& root, const QString& dotPath)
{
    QJsonObject obj = root;
    for (const QString& segment : dotPath.split(QLatin1Char('.'))) {
        obj = obj.value(segment).toObject();
    }
    return obj;
}
} // namespace

class TestMigrationV8ToV9 : public QObject
{
    Q_OBJECT

private:
    void writeIniFile(const QString& path, const QString& content)
    {
        QDir().mkpath(QFileInfo(path).absolutePath());
        QFile f(path);
        QVERIFY(f.open(QIODevice::WriteOnly | QIODevice::Text));
        QTextStream out(&f);
        out << content;
    }

    QJsonObject readJson(const QString& path)
    {
        QFile f(path);
        if (!f.open(QIODevice::ReadOnly)) {
            return {};
        }
        return QJsonDocument::fromJson(f.readAll()).object();
    }

private Q_SLOTS:
    void testStringEncodedMapAndListBecomeNative()
    {
        const QJsonObject profile{{QStringLiteral("duration"), 275}, {QStringLiteral("minDistance"), 9}};
        const QJsonArray triggers{trigger(2), trigger(4)};
        const QJsonArray entries{QJsonObject{{QStringLiteral("id"), QStringLiteral("{a}")}}};

        QJsonObject root;
        root.insert(kVersion, 8);
        root.insert(QStringLiteral("Animations"),
                    QJsonObject{{QStringLiteral("Profile"), compact(profile)}, {QStringLiteral("Enabled"), true}});
        root.insert(QStringLiteral("Snapping"),
                    QJsonObject{{QStringLiteral("Behavior"),
                                 QJsonObject{{QStringLiteral("Triggers"), compact(triggers)},
                                             {QStringLiteral("ZoneSpan"),
                                              QJsonObject{{QStringLiteral("Triggers"), compact(QJsonArray{})}}}}}});
        root.insert(QStringLiteral("Scrolling"),
                    QJsonObject{{QStringLiteral("Wheel"),
                                 QJsonObject{{QStringLiteral("Focus"),
                                              QJsonObject{{QStringLiteral("Triggers"), compact(triggers)}}}}}});
        root.insert(QStringLiteral("Workspaces"),
                    QJsonObject{{QStringLiteral("Named"), QJsonObject{{QStringLiteral("Entries"), compact(entries)}}}});

        ConfigMigration::migrateV8ToV9(root);

        const QJsonValue profileAfter = groupAt(root, QStringLiteral("Animations")).value(QStringLiteral("Profile"));
        QVERIFY2(profileAfter.isObject(), "a string-encoded map must become a native object");
        QCOMPARE(profileAfter.toObject(), profile);

        const QJsonValue triggersAfter =
            groupAt(root, QStringLiteral("Snapping.Behavior")).value(QStringLiteral("Triggers"));
        QVERIFY2(triggersAfter.isArray(), "a string-encoded list must become a native array");
        QCOMPARE(triggersAfter.toArray(), triggers);

        // An empty list is still a list.
        const QJsonValue zoneSpanAfter =
            groupAt(root, QStringLiteral("Snapping.Behavior.ZoneSpan")).value(QStringLiteral("Triggers"));
        QVERIFY(zoneSpanAfter.isArray());
        QVERIFY(zoneSpanAfter.toArray().isEmpty());

        // Deeper dot-path groups are reached too.
        QCOMPARE(groupAt(root, QStringLiteral("Scrolling.Wheel.Focus")).value(QStringLiteral("Triggers")).toArray(),
                 triggers);
        QCOMPARE(groupAt(root, QStringLiteral("Workspaces.Named")).value(QStringLiteral("Entries")).toArray(), entries);

        // Siblings in a rewritten group survive.
        QVERIFY(groupAt(root, QStringLiteral("Animations")).value(QStringLiteral("Enabled")).toBool());
        QCOMPARE(root.value(kVersion).toInt(), 9);
    }

    void testUnparsableOrWrongShapeStringIsLeftAsIs()
    {
        const QString broken = QStringLiteral("{definitely not valid json");
        // Valid JSON, but an array where an object belongs and vice versa.
        const QString arrayForMap = QStringLiteral("[1,2]");
        const QString objectForList = QStringLiteral("{\"modifier\":2}");

        QJsonObject root;
        root.insert(kVersion, 8);
        root.insert(
            QStringLiteral("Animations"),
            QJsonObject{{QStringLiteral("Profile"), broken}, {QStringLiteral("ShaderProfileTree"), arrayForMap}});
        root.insert(
            QStringLiteral("Tiling"),
            QJsonObject{{QStringLiteral("Behavior"), QJsonObject{{QStringLiteral("Triggers"), objectForList}}}});

        ConfigMigration::migrateV8ToV9(root);

        const QJsonObject animations = groupAt(root, QStringLiteral("Animations"));
        QCOMPARE(animations.value(QStringLiteral("Profile")), QJsonValue(broken));
        QCOMPARE(animations.value(QStringLiteral("ShaderProfileTree")), QJsonValue(arrayForMap));
        QCOMPARE(groupAt(root, QStringLiteral("Tiling.Behavior")).value(QStringLiteral("Triggers")),
                 QJsonValue(objectForList));
        QCOMPARE(root.value(kVersion).toInt(), 9);
    }

    void testNativeAndUnrelatedValuesAreUntouched()
    {
        const QJsonObject tree{{QStringLiteral("baseline"), QJsonObject()},
                               {QStringLiteral("overrides"), QJsonArray()}};
        const QJsonObject perAlgorithm{{QStringLiteral("bsp"), QJsonObject{{QStringLiteral("splitRatio"), 0.5}}}};

        QJsonObject root;
        root.insert(kVersion, 8);
        root.insert(QStringLiteral("Animations"), QJsonObject{{QStringLiteral("MotionProfileTree"), tree}});
        root.insert(QStringLiteral("Tiling"),
                    QJsonObject{{QStringLiteral("Algorithm"),
                                 QJsonObject{{QStringLiteral("PerAlgorithmSettings"), perAlgorithm},
                                             // A string-typed key holding JSON-looking text
                                             // is not this step's to touch.
                                             {QStringLiteral("Default"), QStringLiteral("[\"bsp\"]")}}}});
        root.insert(QStringLiteral("Ordering"),
                    QJsonObject{{QStringLiteral("SnappingLayoutOrder"), QStringLiteral("{a},{b}")}});
        const QJsonObject before = root;

        ConfigMigration::migrateV8ToV9(root);

        QJsonObject expected = before;
        expected.insert(kVersion, 9);
        QCOMPARE(root, expected);
    }

    void testAbsentKeysCreateNothing()
    {
        QJsonObject root{{kVersion, 8}};
        ConfigMigration::migrateV8ToV9(root);
        QCOMPARE(root, (QJsonObject{{kVersion, 9}}));
    }

    void testAlreadyAtV9IsLeftAlone()
    {
        // The step's own idempotency guard: a root already at v9 is not this
        // step's, even if it holds a string (it would be the store's
        // malformed case then).
        const QString encoded = compact(QJsonObject{{QStringLiteral("duration"), 100}});
        QJsonObject root;
        root.insert(kVersion, 9);
        root.insert(QStringLiteral("Animations"), QJsonObject{{QStringLiteral("Profile"), encoded}});
        const QJsonObject before = root;

        ConfigMigration::migrateV8ToV9(root);

        QCOMPARE(root, before);
    }

    /// A v1 JSON config from before the store wrote structured values natively
    /// carried its trigger lists as strings, and migrateV1ToV2 moves them
    /// verbatim. The chain must still end with native arrays.
    void testChainFromV1JsonEndsNative()
    {
        // migrateV1ToV2 writes sibling files beside the config, so isolate.
        IsolatedConfigGuard guard;
        const QJsonArray triggers{trigger(2)};
        QJsonObject root;
        root.insert(kVersion, 1);
        root.insert(QStringLiteral("Activation"),
                    QJsonObject{{QStringLiteral("DragActivationTriggers"), compact(triggers)}});
        root.insert(QStringLiteral("Animations"), QJsonObject{{QStringLiteral("AnimationDuration"), 200}});

        ConfigMigration::runMigrationChainInMemory(root, ConfigMigration::ExternalImports::Disabled);

        QCOMPARE(root.value(kVersion).toInt(), ConfigSchemaVersion);
        const QJsonValue triggersAfter =
            groupAt(root, QStringLiteral("Snapping.Behavior")).value(QStringLiteral("Triggers"));
        QVERIFY2(triggersAfter.isArray(), "a v1 string-encoded trigger list must end the chain native");
        QCOMPARE(triggersAfter.toArray(), triggers);
        const QJsonValue profile = groupAt(root, QStringLiteral("Animations")).value(QStringLiteral("Profile"));
        QVERIFY2(profile.isObject(), "migrateV1ToV2's string Profile must end the chain native");
        QCOMPARE(profile.toObject().value(QStringLiteral("duration")).toInt(), 200);
    }

    /// The full chain from a v1 INI through ensureJsonConfig: the stored values
    /// are native, and the settings model reads them through the store, which
    /// no longer parses JSON-encoded strings.
    void testChainFromV1IniEndsNativeAndReadable()
    {
        IsolatedConfigGuard guard;
        writeIniFile(ConfigDefaults::legacyConfigFilePath(),
                     QStringLiteral("[Activation]\n"
                                    "DragActivationTriggers=[{\"modifier\":4,\"mouseButton\":0}]\n"
                                    "\n"
                                    "[Animations]\n"
                                    "AnimationsEnabled=true\n"
                                    "AnimationDuration=275\n"
                                    "AnimationStaggerInterval=42\n"));

        QVERIFY(ConfigMigration::ensureJsonConfig());

        const QJsonObject root = readJson(ConfigDefaults::configFilePath());
        QCOMPARE(root.value(kVersion).toInt(), ConfigSchemaVersion);
        const QJsonValue profile = groupAt(root, QStringLiteral("Animations")).value(QStringLiteral("Profile"));
        QVERIFY2(profile.isObject(), "Animations/Profile must be stored as a native object");
        QCOMPARE(profile.toObject().value(QStringLiteral("duration")).toInt(), 275);
        const QJsonValue triggers =
            groupAt(root, QStringLiteral("Snapping.Behavior")).value(QStringLiteral("Triggers"));
        QVERIFY2(triggers.isArray(), "Snapping.Behavior/Triggers must be stored as a native array");

        Settings settings;
        QCOMPARE(settings.animationDuration(), 275);
        QCOMPARE(settings.animationStaggerInterval(), 42);
        const QVariantList readTriggers = settings.dragActivationTriggers();
        QCOMPARE(readTriggers.size(), 1);
        QCOMPARE(readTriggers.first().toMap().value(QStringLiteral("modifier")).toInt(), 4);
    }
};

QTEST_MAIN(TestMigrationV8ToV9)
#include "test_migration_v8_to_v9.moc"
