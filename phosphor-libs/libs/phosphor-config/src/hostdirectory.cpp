// SPDX-FileCopyrightText: 2026 fuddlesworth
// SPDX-License-Identifier: LGPL-2.1-or-later

#include "PhosphorConfig/HostDirectory.h"

#include <QDir>
#include <QFile>
#include <QFileInfo>
#include <QJsonArray>
#include <QJsonDocument>
#include <QJsonObject>
#include <QLoggingCategory>
#include <QMap>
#include <QRegularExpression>
#include <QSaveFile>
#include <QSet>
#include <QStandardPaths>

#include <algorithm>
#include <utility>

namespace PhosphorConfig {

namespace {
Q_LOGGING_CATEGORY(lcHostDirectory, "phosphorconfig.hostdirectory")

/// A descriptor or a record is a few hundred bytes. A file past this size is
/// not one, and is refused rather than read into memory.
constexpr qint64 kMaxFileBytes = 1024 * 1024;

const QRegularExpression& idPattern()
{
    // anchoredPattern rather than ^ and $: `$` also matches before a trailing
    // newline, which would let "id\n" through.
    static const QRegularExpression pattern(QRegularExpression::anchoredPattern(QStringLiteral("[a-z0-9][a-z0-9-]*")));
    return pattern;
}

QJsonObject readObject(const QString& path, QString* error)
{
    QFile file(path);
    if (!file.open(QIODevice::ReadOnly)) {
        *error = file.errorString();
        return {};
    }
    const QByteArray bytes = file.read(kMaxFileBytes + 1);
    if (bytes.size() > kMaxFileBytes) {
        *error = QStringLiteral("larger than %1 bytes").arg(kMaxFileBytes);
        return {};
    }
    QJsonParseError parseError;
    const QJsonDocument document = QJsonDocument::fromJson(bytes, &parseError);
    if (!document.isObject()) {
        *error = parseError.error != QJsonParseError::NoError ? parseError.errorString()
                                                              : QStringLiteral("not a JSON object");
        return {};
    }
    return document.object();
}

/// Whether @p text holds an ASCII control character: U+0000 to U+001F, which
/// covers the line breaks, or U+007F.
bool hasControlCharacter(const QString& text)
{
    return std::any_of(text.cbegin(), text.cend(), [](QChar character) {
        return character.unicode() < 0x20 || character.unicode() == 0x7F;
    });
}

/// The file a descriptor's @p configPath names for this user. Empty, with
/// @p error saying why, when it names none.
QString resolveConfigPath(const QString& configPath, QString* error)
{
    const QList<std::pair<QLatin1String, QStandardPaths::StandardLocation>> roots = {
        {QLatin1String("$XDG_CONFIG_HOME"), QStandardPaths::GenericConfigLocation},
        {QLatin1String("$XDG_DATA_HOME"), QStandardPaths::GenericDataLocation},
        {QLatin1String("$XDG_STATE_HOME"), QStandardPaths::GenericStateLocation},
        {QLatin1String("$HOME"), QStandardPaths::HomeLocation},
    };
    for (const auto& [variable, location] : roots) {
        if (!configPath.startsWith(variable + QLatin1Char('/'))) {
            continue;
        }
        const QString rest = configPath.mid(variable.size());
        // Every segment after the variable. A trailing slash and a doubled
        // one both leave an empty segment, and neither names a file.
        const QStringList segments = rest.mid(1).split(QLatin1Char('/'));
        if (segments.contains(QString()) || segments.contains(QLatin1String("."))
            || segments.contains(QLatin1String("..")) || hasControlCharacter(rest)) {
            break;
        }
        const QString root = QStandardPaths::writableLocation(location);
        if (root.isEmpty() || !QDir::isAbsolutePath(root)) {
            *error = QStringLiteral("\"ConfigPath\" %1 cannot be resolved: %2 expands to %3, which is not absolute")
                         .arg(configPath, variable, root);
            return QString();
        }
        return QDir::cleanPath(root + rest);
    }
    *error = QStringLiteral(
                 "\"ConfigPath\" %1 must be $XDG_CONFIG_HOME, $XDG_DATA_HOME, $XDG_STATE_HOME or $HOME "
                 "followed by a path with no empty, . or .. segment and no control character")
                 .arg(configPath);
    return QString();
}

/// Whether @p path can be a record's config path: absolute on the file
/// system, with no `..` segment and no control character. A leading slash is
/// the test, because QDir::isAbsolutePath() also accepts a `:/` resource path.
bool isRecordablePath(const QString& path)
{
    return path.startsWith(QLatin1Char('/')) && !path.split(QLatin1Char('/')).contains(QLatin1String(".."))
        && !hasControlCharacter(path);
}

/// Whether one of @p currentDesktops is among the desktops @p descriptor
/// declares, ignoring case.
bool servesOneOf(const HostDescriptor& descriptor, const QStringList& currentDesktops)
{
    return std::any_of(currentDesktops.cbegin(), currentDesktops.cend(), [&](const QString& desktop) {
        return descriptor.desktops.contains(desktop, Qt::CaseInsensitive);
    });
}
} // namespace

std::optional<HostDescriptor> HostDescriptor::parse(const QJsonObject& json, QString* error)
{
    const auto fail = [error](const QString& message) -> std::optional<HostDescriptor> {
        if (error) {
            *error = message;
        }
        return std::nullopt;
    };
    HostDescriptor descriptor;
    descriptor.id = json.value(QLatin1String("Id")).toString();
    descriptor.name = json.value(QLatin1String("Name")).toString();
    descriptor.busName = json.value(QLatin1String("BusName")).toString();
    descriptor.backend = json.value(QLatin1String("Backend")).toString();
    descriptor.configPath = json.value(QLatin1String("ConfigPath")).toString();
    if (!idPattern().match(descriptor.id).hasMatch()) {
        return fail(QStringLiteral("\"Id\" must match [a-z0-9][a-z0-9-]*"));
    }
    if (descriptor.name.isEmpty()) {
        return fail(QStringLiteral("\"Name\" must be a non-empty string"));
    }
    if (descriptor.busName.isEmpty()) {
        return fail(QStringLiteral("\"BusName\" must be a non-empty string"));
    }
    if (descriptor.backend != QLatin1String("json")) {
        return fail(QStringLiteral("\"Backend\" %1 is not a known backend (json)").arg(descriptor.backend));
    }
    QString pathError;
    if (resolveConfigPath(descriptor.configPath, &pathError).isEmpty()) {
        return fail(pathError);
    }
    if (json.contains(QLatin1String("Desktops"))) {
        const QJsonValue desktops = json.value(QLatin1String("Desktops"));
        if (!desktops.isArray()) {
            return fail(QStringLiteral("\"Desktops\" must be an array of non-empty strings"));
        }
        for (const QJsonValue& desktop : desktops.toArray()) {
            if (!desktop.isString() || desktop.toString().isEmpty()) {
                return fail(QStringLiteral("\"Desktops\" must be an array of non-empty strings"));
            }
            descriptor.desktops.append(desktop.toString());
        }
    }
    return descriptor;
}

QString HostDescriptor::resolvedConfigPath() const
{
    QString error;
    return resolveConfigPath(configPath, &error);
}

HostDirectory::HostDirectory(QString api)
    : m_api(std::move(api))
{
    // The name becomes part of a directory name and a file name, so one that
    // is not an id (`../x`) is dropped here: descriptors() and recordPath()
    // answer nothing for an empty name.
    if (!idPattern().match(m_api).hasMatch()) {
        qCWarning(lcHostDirectory) << "HostDirectory: ignoring the invalid API name" << m_api;
        m_api.clear();
    }
}

QList<HostDescriptor> HostDirectory::descriptors() const
{
    if (m_api.isEmpty()) {
        return {};
    }
    const QString subdirectory = QStringLiteral("phosphor-works/%1-hosts").arg(m_api);
    // locateAll lists the per-user directory first, so the first descriptor
    // seen for an id is the one that wins.
    QMap<QString, HostDescriptor> byId;
    const QStringList directories =
        QStandardPaths::locateAll(QStandardPaths::GenericDataLocation, subdirectory, QStandardPaths::LocateDirectory);
    // XDG_DATA_DIRS can repeat XDG_DATA_HOME, and two entries can be links to
    // one directory. Each directory is read once, or its files would be
    // reported as overridden by themselves.
    QSet<QString> scanned;
    for (const QString& directory : directories) {
        const QString canonical = QFileInfo(directory).canonicalFilePath();
        if (scanned.contains(canonical)) {
            continue;
        }
        scanned.insert(canonical);
        if (!QFileInfo(directory).isReadable()) {
            qCWarning(lcHostDirectory) << "HostDirectory: skipping the unreadable directory" << directory;
            continue;
        }
        // CaseSensitive: name filters ignore case otherwise, and the schema
        // gate only checks files named `*.json`.
        const QFileInfoList files =
            QDir(directory).entryInfoList({QStringLiteral("*.json")}, QDir::Files | QDir::CaseSensitive, QDir::Name);
        for (const QFileInfo& file : files) {
            QString error;
            const QJsonObject json = readObject(file.absoluteFilePath(), &error);
            std::optional<HostDescriptor> descriptor;
            if (error.isEmpty()) {
                descriptor = HostDescriptor::parse(json, &error);
            }
            if (!descriptor) {
                qCWarning(lcHostDirectory) << "HostDirectory: skipping" << file.absoluteFilePath() << "-" << error;
                continue;
            }
            if (byId.contains(descriptor->id)) {
                qCDebug(lcHostDirectory) << "HostDirectory:" << file.absoluteFilePath()
                                         << "is overridden by an earlier descriptor for" << descriptor->id;
                continue;
            }
            byId.insert(descriptor->id, *descriptor);
        }
    }
    return byId.values();
}

QString HostDirectory::recordPath() const
{
    const QString root = QStandardPaths::writableLocation(QStandardPaths::GenericStateLocation);
    // A relative HOME gives a relative state directory, and a record under it
    // would land wherever the process happens to run.
    if (m_api.isEmpty() || !QDir::isAbsolutePath(root)) {
        return QString();
    }
    return root + QStringLiteral("/phosphor-works/%1-host.json").arg(m_api);
}

std::optional<HostRecord> HostDirectory::record() const
{
    const QString path = recordPath();
    if (!QFile::exists(path)) {
        return std::nullopt;
    }
    QString error;
    const QJsonObject json = readObject(path, &error);
    HostRecord record;
    if (error.isEmpty()) {
        record.host = json.value(QLatin1String("Host")).toString();
        record.configPath = json.value(QLatin1String("ConfigPath")).toString();
        // -1 for a missing Pid, one that is not a number and one that is not
        // an integer, so none of them reads as 0, the pid of a user's choice.
        record.pid = json.value(QLatin1String("Pid")).toInteger(-1);
        record.written = QDateTime::fromString(json.value(QLatin1String("Written")).toString(), Qt::ISODate);
        if (!idPattern().match(record.host).hasMatch()) {
            error = QStringLiteral("Host must match [a-z0-9][a-z0-9-]*");
        } else if (!isRecordablePath(record.configPath)) {
            error = QStringLiteral("ConfigPath must be an absolute path with no .. segment or control character");
        } else if (record.pid < 0) {
            error = QStringLiteral("Pid must be a non-negative integer");
        } else if (!record.written.isValid()) {
            error = QStringLiteral("Written must be an ISO date");
        }
    }
    if (!error.isEmpty()) {
        qCWarning(lcHostDirectory) << "HostDirectory: ignoring the unreadable host record" << path << "-" << error;
        return std::nullopt;
    }
    return record;
}

bool HostDirectory::writeRecord(const QString& hostId, const QString& configPath, qint64 pid) const
{
    const QString path = recordPath();
    if (path.isEmpty()) {
        qCWarning(lcHostDirectory) << "HostDirectory: refusing to record host" << hostId
                                   << "- the API name is invalid or the state directory is not absolute";
        return false;
    }
    if (!idPattern().match(hostId).hasMatch() || !isRecordablePath(configPath) || pid < 0) {
        qCWarning(lcHostDirectory) << "HostDirectory: refusing to record host" << hostId << "with config path"
                                   << configPath << "and pid" << pid;
        return false;
    }
    QJsonObject json;
    json.insert(QLatin1String("Host"), hostId);
    json.insert(QLatin1String("ConfigPath"), QDir::cleanPath(configPath));
    json.insert(QLatin1String("Pid"), pid);
    json.insert(QLatin1String("Written"), QDateTime::currentDateTimeUtc().toString(Qt::ISODate));
    QDir().mkpath(QFileInfo(path).absolutePath());
    QSaveFile file(path);
    if (!file.open(QIODevice::WriteOnly) || file.write(QJsonDocument(json).toJson()) < 0 || !file.commit()) {
        qCWarning(lcHostDirectory) << "HostDirectory: could not write" << path << "-" << file.errorString();
        return false;
    }
    return true;
}

HostResolution HostDirectory::resolve(const QList<HostDescriptor>& descriptors, const std::optional<HostRecord>& record,
                                      qint64 busOwnerPid, const QStringList& currentDesktops)
{
    HostResolution resolution;
    resolution.running = busOwnerPid > 0;

    if (record) {
        const auto named =
            std::find_if(descriptors.cbegin(), descriptors.cend(), [&](const HostDescriptor& descriptor) {
                return descriptor.id == record->host;
            });
        if (named != descriptors.cend()) {
            // The running host's own record: the process that wrote it owns
            // the name now.
            const bool writtenByTheOwner = busOwnerPid > 0 && record->pid == busOwnerPid;
            // A user's choice between hosts counts whether or not a host
            // runs. A host's record counts once nothing owns the name: while
            // another process owns it, that record is stale.
            const bool usersChoice = record->pid == 0;
            const bool nothingOwnsTheName = busOwnerPid <= 0;
            // A record does not cross desktops: its host has to serve this
            // session. Only a host that declares desktops, in a session that
            // names one, can fail that.
            const bool forAnotherDesktop =
                !named->desktops.isEmpty() && !currentDesktops.isEmpty() && !servesOneOf(*named, currentDesktops);
            if (writtenByTheOwner || ((usersChoice || nothingOwnsTheName) && !forAnotherDesktop)) {
                resolution.host = *named;
                resolution.configPath = record->configPath;
                return resolution;
            }
        }
    }

    for (const HostDescriptor& descriptor : descriptors) {
        if (servesOneOf(descriptor, currentDesktops)) {
            resolution.candidates.append(descriptor);
        }
    }
    if (resolution.candidates.size() == 1) {
        resolution.host = resolution.candidates.takeFirst();
        resolution.configPath = resolution.host->resolvedConfigPath();
    }
    return resolution;
}

QStringList HostDirectory::currentDesktops()
{
    return qEnvironmentVariable("XDG_CURRENT_DESKTOP").split(QLatin1Char(':'), Qt::SkipEmptyParts);
}

} // namespace PhosphorConfig
