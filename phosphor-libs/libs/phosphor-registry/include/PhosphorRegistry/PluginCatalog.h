// SPDX-FileCopyrightText: 2026 fuddlesworth
// SPDX-License-Identifier: LGPL-2.1-or-later
#pragma once

#include <PhosphorRegistry/phosphorregistry_export.h>

#include <QHash>
#include <QJsonObject>
#include <QList>
#include <QObject>
#include <QSet>
#include <QString>
#include <QStringList>
#include <QtCore/qtclasshelpermacros.h>

#include <memory>
#include <optional>

namespace PhosphorFsLoader {
class WatchedDirectorySet;
}

namespace PhosphorRegistry {

/// One kind of plugin the suite loads, such as bar widgets.
///
/// A plugin of a seam is a Qt plugin (`qt_add_plugin` + `Q_PLUGIN_METADATA`)
/// whose IID equals `iid` and whose embedded metadata JSON carries at least
/// `"Id"` (a non-empty `[A-Za-z0-9][A-Za-z0-9._-]*` string) and an integer
/// `"ApiVersion"` equal to `apiVersion`. Everything else in that JSON belongs
/// to the seam and is handed through untouched.
///
/// `name` follows the same pattern as an id, `iid` is non-empty and
/// `apiVersion` is at least 1. A catalog over a seam that breaks any of these
/// logs a warning and offers nothing.
struct PHOSPHORREGISTRY_EXPORT PluginSeam
{
    /// Directory name under each plugin root, for example `bar-widget`.
    QString name;
    /// The IID every plugin of this seam declares, for example
    /// `org.phosphor_works.Lambent.BarWidget/1`.
    QString iid;
    /// The `ApiVersion` this build loads. A plugin built for another one is
    /// refused before its library is loaded.
    int apiVersion = 0;
};

/// Where a plugin was found. Earlier origins override later ones.
enum class PluginOrigin : quint8 {
    /// A development root: a directory from PHOSPHOR_WORKS_PLUGIN_PATH, or
    /// one a host passes to setSearchPaths, for running plugins straight from
    /// a build tree.
    Development,
    /// `$XDG_DATA_HOME/phosphor-works/plugins/<seam>`.
    User,
    /// A Qt plugin directory plus `/phosphor-works/<seam>`, where packages
    /// install.
    System,
};

/// A directory to search and the origin its plugins get.
struct PHOSPHORREGISTRY_EXPORT PluginSearchPath
{
    QString directory;
    PluginOrigin origin = PluginOrigin::System;
};

/// A plugin known from its metadata. Building one never loads the library.
struct PHOSPHORREGISTRY_EXPORT PluginMetadata
{
    QString id;
    /// Absolute path of the plugin library.
    QString filePath;
    PluginOrigin origin = PluginOrigin::System;
    /// The plugin's own metadata JSON, as given to Q_PLUGIN_METADATA.
    QJsonObject metaData;
};

/// A plugin file the catalog found and will not offer, with the reason.
struct PHOSPHORREGISTRY_EXPORT RefusedPlugin
{
    QString filePath;
    /// The id from its metadata, or empty when the refusal came first.
    QString id;
    QString reason;
};

/// Finds the plugins of one seam, reads their metadata without loading them,
/// and loads one only when asked.
///
/// Search order, highest priority first (see defaultSearchPaths):
///   1. each directory in PHOSPHOR_WORKS_PLUGIN_PATH, plus `/<seam>`
///   2. `$XDG_DATA_HOME/phosphor-works/plugins/<seam>`
///   3. each Qt plugin directory (QT_PLUGIN_PATH, then Qt's own), plus
///      `/phosphor-works/<seam>`
/// A plugin found earlier overrides one with the same id found later, and the
/// override is logged.
///
/// Every `*.so` file in a searched directory is vetted, and refused with the
/// reason kept in refused() and logged once, when:
///   - it is a symlink, or not a regular file, or not readable
///   - it or its directory is group- or world-writable, or owned by someone
///     other than this user or root
///   - a `<file>.avx2` sibling exists, which Qt would load in its place
///   - it is not a Qt plugin, or its IID is not the seam's
///   - it is built against a Qt this process cannot load
///   - its metadata has no valid `"Id"`, or no integer `"ApiVersion"`
///   - its `"ApiVersion"` is not the seam's
///   - another plugin in the same directory has the same id
/// The file is vetted again right before load() maps it. Only the plugin file
/// and its directory are vetted: libraries the plugin links or opens are its
/// own trust decision. Hidden files are not considered.
///
/// Loaded plugins are never unloaded. A loaded plugin whose file is removed
/// or now refused leaves plugins() and emits pluginRemoved, even when another
/// copy with the same id exists (that copy takes effect at the next start),
/// and its loaded instance stays valid. A loaded plugin keeps running the code
/// it was loaded from until the process restarts, and Qt keeps reporting the
/// metadata of that code for its path. Replace a loaded plugin by rename
/// (`install`, `mv`), never by writing over it in place, which changes the
/// code the process is running.
///
/// A plugin on offer that is not loaded is offered again, with a new
/// pluginAdded for an id already in plugins(), once the file behind it is no
/// longer the one it was last offered with: rewritten, replaced by a rename
/// over it, overridden by a copy found earlier, or removed while another copy
/// of the same id takes its place. A failed load() counts as
/// an offer of the file as it was then, so a plugin whose load failed is
/// offered again only once its file changes after that. A file is told apart
/// by its device, inode, size and modification time. A scan that finds every
/// such file as it was emits neither pluginAdded nor pluginRemoved. A loaded
/// plugin is never offered again this way, since it keeps the code it was
/// loaded from.
///
/// Calling scan() from a slot that a scan's own signals reach defers the
/// rescan until the running one has finished.
///
/// GUI thread only, like WatchedDirectorySet.
class PHOSPHORREGISTRY_EXPORT PluginCatalog : public QObject
{
    Q_OBJECT
public:
    /// The environment variable holding development plugin roots,
    /// colon-separated. Each root holds one subdirectory per seam.
    static constexpr const char* DevelopmentPathVariable = "PHOSPHOR_WORKS_PLUGIN_PATH";

    explicit PluginCatalog(PluginSeam seam, QObject* parent = nullptr);
    ~PluginCatalog() override;
    Q_DISABLE_COPY_MOVE(PluginCatalog)

    /// The directories searched for @p seamName when no search paths are set,
    /// highest priority first. Relative entries are skipped with a warning.
    [[nodiscard]] static QList<PluginSearchPath> defaultSearchPaths(const QString& seamName);

    [[nodiscard]] const PluginSeam& seam() const;

    /// Replace the directories to search, highest priority first. A relative
    /// directory is resolved against the current working directory now. Must
    /// be called before scan(). An empty list, or no call at all, means
    /// defaultSearchPaths.
    void setSearchPaths(const QList<PluginSearchPath>& paths);
    [[nodiscard]] QList<PluginSearchPath> searchPaths() const;

    /// Watch the search paths and rescan when they change (on by default).
    /// Must be called before scan(). A search path that does not exist yet is
    /// picked up when it appears, unless its nearest existing ancestor is a
    /// directory the watcher refuses to watch (such as `~/.local/share` or
    /// `/tmp`). Then it is picked up by the next scan().
    void setLiveReload(bool enabled);

    /// Scan the search paths and read every plugin's metadata. The first call
    /// registers the directories, arms the watch and scans synchronously.
    /// Later calls rescan synchronously, except from inside a running scan.
    void scan();

    /// Every plugin on offer, sorted by id.
    [[nodiscard]] QList<PluginMetadata> plugins() const;
    [[nodiscard]] std::optional<PluginMetadata> plugin(const QString& id) const;
    /// The plugin files refused by the last scan, sorted by path.
    [[nodiscard]] QList<RefusedPlugin> refused() const;

    /// Load the plugin @p id, which must be on offer, and return its root
    /// object, or nullptr with the reason in @p errorString. The file is
    /// vetted again first. When it no longer passes, load() fails and
    /// schedules a rescan, live reload or not, which withdraws the plugin.
    /// A library with an unresolved symbol fails to load. A later call
    /// returns the same object, also after the plugin was withdrawn and
    /// offered again.
    QObject* load(const QString& id, QString* errorString = nullptr);
    [[nodiscard]] bool isLoaded(const QString& id) const;

    /// Test seam: cap the number of plugin files considered per scan. A scan
    /// that reaches the cap keeps what it knew about the files it did not
    /// reach. Zero or negative restores the default.
    void setMaxFilesPerScanForTest(int cap);

Q_SIGNALS:
    /// A plugin id came on offer, or the file of a plugin on offer that is
    /// not loaded changed (see the class notes). A slot can therefore see an
    /// id it already knows, and should read plugin() again.
    void pluginAdded(const QString& id);
    /// A plugin id left the offer: its file is gone or now refused.
    void pluginRemoved(const QString& id);
    /// After every scan, whether or not anything changed.
    void scanCompleted();

private:
    class ScanStrategy;
    struct Loaded;
    struct Vetted;

    /// What a file was when it was offered, or when a load of it failed, to
    /// notice when it changes.
    struct FileIdentity
    {
        quint64 device = 0;
        quint64 inode = 0;
        qint64 size = 0;
        qint64 modifiedNs = 0;
        bool operator==(const FileIdentity&) const = default;
    };

    QStringList performScan(const QStringList& directoriesLowestPriorityFirst);
    void forwardScanCompleted();
    [[nodiscard]] Vetted vet(const QString& filePath, PluginOrigin origin) const;
    [[nodiscard]] static FileIdentity identityOf(const QString& filePath);
    [[nodiscard]] bool shouldWarn(const QString& key);
    [[nodiscard]] PluginOrigin originOf(const QString& directory) const;

    PluginSeam m_seam;
    bool m_seamValid = false;
    QList<PluginSearchPath> m_searchPaths;
    bool m_liveReload = true;
    bool m_registered = false;
    /// Nesting depth of performScan and of the scanCompleted emit. scan()
    /// defers while it is non-zero.
    int m_scanDepth = 0;
    int m_maxFilesPerScan = 0;
    std::unique_ptr<ScanStrategy> m_strategy;
    std::unique_ptr<PhosphorFsLoader::WatchedDirectorySet> m_watcher;
    QHash<QString, PluginMetadata> m_plugins;
    QList<RefusedPlugin> m_refused;
    QHash<QString, std::shared_ptr<Loaded>> m_loaded;
    /// Every plugin on offer that is not loaded, with its file as it was when
    /// it was last offered or a load of it last failed.
    QHash<QString, FileIdentity> m_offered;
    /// Warn-once keys. A key is dropped when its file stops producing that
    /// warning, so a regression warns again.
    QSet<QString> m_warned;
};

} // namespace PhosphorRegistry
