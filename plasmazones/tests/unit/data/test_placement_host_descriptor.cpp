// SPDX-FileCopyrightText: 2026 fuddlesworth
// SPDX-License-Identifier: GPL-3.0-or-later

// Author-time checks on data/placement-hosts/plasmazones.json, read straight
// out of the source tree and run through the parser a settings module uses at
// run time (PhosphorConfig::HostDescriptor::parse).
//
// The descriptor repeats, as hand-typed strings, values the daemon takes from
// constants: the host id it records itself under (PlasmaZones::PlacementHostId),
// the bus name it claims (PhosphorProtocol::Service::Name) and the file its
// settings live in (ConfigDefaults::configFilePath()). Nothing else ties the
// JSON to them. A rename on either side would leave
// PhosphorConfig::HostDirectory::resolve() with a record that names no
// descriptor, or point a settings module at a file the daemon does not read,
// and neither failure is loud. Each case below pins one of the values.

#include <QtTest>

#include "config/configdefaults.h"
#include "core/types/constants.h"

#include <PhosphorConfig/HostDirectory.h>
#include <PhosphorProtocol/ServiceConstants.h>

#include <QDir>
#include <QFile>
#include <QJsonDocument>
#include <QJsonObject>

#include <optional>

namespace {

/// The shipped descriptor, parsed. Disengaged with @p why set when the file
/// cannot be read, is not a JSON object, or the runtime parser refuses it.
std::optional<PhosphorConfig::HostDescriptor> loadShippedDescriptor(QString* why)
{
    const QString path = QStringLiteral(P_SOURCE_DIR "/data/placement-hosts/plasmazones.json");
    QFile file(path);
    if (!file.open(QIODevice::ReadOnly)) {
        *why = QStringLiteral("cannot open %1: %2").arg(path, file.errorString());
        return std::nullopt;
    }
    QJsonParseError parseError;
    const QJsonDocument doc = QJsonDocument::fromJson(file.readAll(), &parseError);
    if (parseError.error != QJsonParseError::NoError || !doc.isObject()) {
        *why = QStringLiteral("%1 is not a JSON object: %2").arg(path, parseError.errorString());
        return std::nullopt;
    }
    return PhosphorConfig::HostDescriptor::parse(doc.object(), why);
}

} // namespace

class TestPlacementHostDescriptor : public QObject
{
    Q_OBJECT

private Q_SLOTS:
    void theRuntimeParserAcceptsIt();
    void idIsTheOneTheDaemonRecords();
    void busNameIsThePlacementService();
    void configPathIsTheDaemonsConfigFile();
};

void TestPlacementHostDescriptor::theRuntimeParserAcceptsIt()
{
    QString why;
    const auto descriptor = loadShippedDescriptor(&why);
    QVERIFY2(descriptor.has_value(), qPrintable(why));
}

void TestPlacementHostDescriptor::idIsTheOneTheDaemonRecords()
{
    QString why;
    const auto descriptor = loadShippedDescriptor(&why);
    QVERIFY2(descriptor.has_value(), qPrintable(why));
    QCOMPARE(descriptor->id, QString(PlasmaZones::PlacementHostId));
}

void TestPlacementHostDescriptor::busNameIsThePlacementService()
{
    QString why;
    const auto descriptor = loadShippedDescriptor(&why);
    QVERIFY2(descriptor.has_value(), qPrintable(why));
    QCOMPARE(descriptor->busName, QString(PhosphorProtocol::Service::Name));
}

void TestPlacementHostDescriptor::configPathIsTheDaemonsConfigFile()
{
    QString why;
    const auto descriptor = loadShippedDescriptor(&why);
    QVERIFY2(descriptor.has_value(), qPrintable(why));
    // Both sides resolve the config root through QStandardPaths, so they agree
    // under the test sandbox's XDG_CONFIG_HOME as they do under a user's.
    // resolvedConfigPath() cleans its answer, and ConfigDefaults joins its
    // path by hand.
    QCOMPARE(descriptor->resolvedConfigPath(), QDir::cleanPath(PlasmaZones::ConfigDefaults::configFilePath()));
}

QTEST_MAIN(TestPlacementHostDescriptor)
#include "test_placement_host_descriptor.moc"
