// SPDX-FileCopyrightText: 2026 fuddlesworth
// SPDX-License-Identifier: LGPL-2.1-or-later

#pragma once

#include "phosphorconfig_export.h"

#include <QDateTime>
#include <QList>
#include <QString>
#include <QStringList>

#include <optional>

QT_BEGIN_NAMESPACE
class QJsonObject;
QT_END_NAMESPACE

namespace PhosphorConfig {

/// One host of a settings API, as the static descriptor its package installs
/// in `<datadir>/phosphor-works/<api>-hosts/` describes it:
///
///     { "Id": "plasmazones", "Name": "PlasmaZones", "BusName": "org.phosphor_works.Placement",
///       "Backend": "json", "ConfigPath": "$XDG_CONFIG_HOME/plasmazones/config.json",
///       "Desktops": ["KDE"] }
///
/// Several hosts can serve one API, each keeping its settings in its own
/// file. PlasmaZones serves the placement API on KWin, and the Phosphor
/// compositor is the intended second host. A settings module reads the
/// descriptors to find the file to edit whether or not the host runs.
struct PHOSPHORCONFIG_EXPORT HostDescriptor
{
    /// Matches `[a-z0-9][a-z0-9-]*`. Descriptors are keyed by this, not by
    /// the name of the file that holds them.
    QString id;
    /// A product name, shown untranslated where a user chooses between hosts.
    QString name;
    /// The bus name the host owns while it runs. HostDirectory::resolve()
    /// takes the pid of one owner, so the hosts of one API are expected to
    /// share this name. Nothing checks that they do.
    QString busName;
    /// The PhosphorConfig backend the file uses. Only `json` is known.
    QString backend;
    /// The file: `$XDG_CONFIG_HOME`, `$XDG_DATA_HOME`, `$XDG_STATE_HOME` or
    /// `$HOME`, then `/` and a path with no empty, `.` or `..` segment and no
    /// ASCII control character. See resolvedConfigPath().
    QString configPath;
    /// `XDG_CURRENT_DESKTOP` entries the host serves, compared ignoring case.
    /// A host that declares none is never chosen or offered for a session's
    /// desktop. Only a record reaches it. See HostDirectory::resolve().
    QStringList desktops;

    /// Parse @p json. Returns nullopt and sets @p error when a field is
    /// missing or invalid. `Desktops` may be left out, and a key that is not
    /// one of the descriptor's is ignored.
    [[nodiscard]] static std::optional<HostDescriptor> parse(const QJsonObject& json, QString* error);

    /// configPath with its leading variable expanded for this user. Empty
    /// when the variable is unknown, the result is not absolute, or it
    /// contains a `.` or `..` segment. Also empty when no path follows the variable,
    /// a segment of it is empty, or it holds an ASCII control character
    /// (U+0000 to U+001F, or U+007F).
    [[nodiscard]] QString resolvedConfigPath() const;
};

/// What the last host of an API to start wrote about itself, or the choice
/// a user made between several hosts (pid 0).
struct PHOSPHORCONFIG_EXPORT HostRecord
{
    QString host;
    QString configPath;
    qint64 pid = 0;
    QDateTime written;
};

/// Which host's settings file a module edits, from HostDirectory::resolve().
struct PHOSPHORCONFIG_EXPORT HostResolution
{
    /// The host, or nullopt when none could be chosen: no record chose one
    /// and the session's desktops match several descriptors (see candidates)
    /// or none.
    std::optional<HostDescriptor> host;
    /// The file to edit. When a record chose the host it is the record's
    /// path as it was written, which is not compared with the descriptor's
    /// and is stale if the host has moved its file since. Otherwise it is the
    /// descriptor's resolved path. Empty when there is no host.
    QString configPath;
    /// Several descriptors match this session and nothing chose between
    /// them: ask the user, then writeRecord() the answer with pid 0. Empty
    /// when a host was chosen, and also when no descriptor matches the
    /// session's desktops, where host is nullopt with nothing to offer.
    QList<HostDescriptor> candidates;
    /// Some process owns the API's bus name. That process is known to be
    /// the resolved host only when the record that chose the host was written
    /// by it, which is the one case where the owner is sure to be the process
    /// to ask to reload after a write. Under every other rule the owner can
    /// be a different host.
    ///
    /// Owning the name is not the same as answering on it. A host takes its
    /// bus name, and PlasmaZones writes its record, before it registers the
    /// objects behind the name, so for a moment at start a call to the
    /// owner fails with `org.freedesktop.DBus.Error.UnknownObject` although
    /// running is true and the host's fresh record already satisfies rule 1
    /// of resolve(). A module that asks the host to reload should retry such
    /// a reply for a short while rather than read it as the host not running.
    bool running = false;
};

/// The descriptors and the host record of one settings API.
///
/// A running host calls writeRecord() at start. The record stays after it
/// stops, so a settings module knows which file this user last used. A
/// settings module calls descriptors(), record() and resolve() to find the
/// file to edit.
class PHOSPHORCONFIG_EXPORT HostDirectory
{
public:
    /// @p api names the API, `placement` for the placement API. It must match
    /// `[a-z0-9][a-z0-9-]*`. Any other name is refused with a warning:
    /// descriptors() then finds nothing, recordPath() is empty and
    /// writeRecord() refuses.
    explicit HostDirectory(QString api);

    /// Every valid descriptor in the `phosphor-works/<api>-hosts/`
    /// directories of the XDG data dirs, sorted by id. Only files named
    /// `*.json`, the extension in lower case, are read. Descriptors are keyed
    /// by the `Id` inside the file, and the file name is not checked against
    /// it. A per-user descriptor overrides a system one with the same id, and
    /// of two in one directory the first in file name order is kept. Invalid
    /// ones, and directories that cannot be read, are skipped with a warning.
    [[nodiscard]] QList<HostDescriptor> descriptors() const;

    /// `$XDG_STATE_HOME/phosphor-works/<api>-host.json`. Empty when the state
    /// directory is not absolute or the API name was refused: record() then
    /// finds nothing and writeRecord() refuses.
    [[nodiscard]] QString recordPath() const;
    /// The record, or nullopt when there is none. A record is ignored with a
    /// warning unless its `Host` is a valid id, its `ConfigPath` is absolute
    /// with no `..` segment and no ASCII control character, its `Pid` is a
    /// non-negative integer and its `Written` is an ISO date.
    [[nodiscard]] std::optional<HostRecord> record() const;
    /// Record host @p hostId, keeping its settings in @p configPath, as this
    /// user's host of the API. A running host passes its own pid at start; a
    /// user's choice between hosts passes 0 and the descriptor's
    /// resolvedConfigPath(). Refused when @p hostId is not a valid id,
    /// @p configPath is not absolute or has a `..` segment or an ASCII
    /// control character, or @p pid is negative. The path is stored cleaned.
    [[nodiscard]] bool writeRecord(const QString& hostId, const QString& configPath, qint64 pid) const;

    /// Choose a host. The first of these that applies decides:
    ///   1. the running host's own record: the record was written by the
    ///      process that owns the bus name now (@p busOwnerPid, 0 when the
    ///      name has no owner). It wins whatever the desktop.
    ///   2. a user's choice: a record with pid 0 whose host serves this
    ///      session, whether or not anything owns the name
    ///   3. a host's record: a record with any other pid, when nothing owns
    ///      the name and its host serves this session
    ///   4. the one descriptor whose desktops match @p currentDesktops
    ///   5. none. candidates lists every descriptor that matches, for the
    ///      user to choose from. It is empty when no descriptor matches.
    /// A record's host serves this session when one of @p currentDesktops is
    /// among its desktops, ignoring case. A host that declares no desktops
    /// serves every session for this purpose, and an empty
    /// @p currentDesktops is served by every host. The desktop match is
    /// stricter: it never chooses or offers a descriptor without desktops,
    /// and it matches nothing when @p currentDesktops is empty.
    /// A record naming a host with no descriptor is ignored. So is a host's
    /// record while another process owns the name.
    /// Pid equality is the only link between the record and the bus owner:
    /// nothing else ties the process that owns the name to the host the
    /// record names.
    ///
    /// The two pids are counted in different places. The record holds the
    /// writer's own pid (its getpid()), while @p busOwnerPid is what the bus
    /// reports (`GetConnectionUnixProcessID`), which the bus takes from the
    /// kernel's peer credentials and so counts in the bus's own pid
    /// namespace. For a host in another pid namespace (a container, or a
    /// Flatpak sandbox, whose bus traffic also passes through a proxy
    /// process) the two never agree. Rule 1 then never applies, the running
    /// host's own record is read as one written by another process and
    /// ignored, and the choice falls to rule 4. This is a limit of the pid
    /// link, not something resolve() corrects.
    /// @p busOwnerPid is the owner of one bus name, so the descriptors of
    /// one API are expected to share HostDescriptor::busName.
    [[nodiscard]] static HostResolution resolve(const QList<HostDescriptor>& descriptors,
                                                const std::optional<HostRecord>& record, qint64 busOwnerPid,
                                                const QStringList& currentDesktops);

    /// `XDG_CURRENT_DESKTOP`, split on `:`.
    [[nodiscard]] static QStringList currentDesktops();

private:
    QString m_api;
};

} // namespace PhosphorConfig
