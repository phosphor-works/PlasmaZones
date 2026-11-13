// SPDX-FileCopyrightText: 2026 fuddlesworth
// SPDX-License-Identifier: LGPL-2.1-or-later
#pragma once

// The config backends ModuleHost shares among its modules
// (ModuleContext::configBackend). Private to the library.

#include <QHash>
#include <QObject>
#include <QString>

#include <functional>
#include <memory>

namespace PhosphorConfig {
class IBackend;
}

namespace PhosphorControl {

/// One config backend for each file the host's modules open, keyed by the
/// file the OS opens for the path a module named.
///
/// A child of the ApplicationController rather than a member of the host, so
/// a backend is still there when the controller applies a domain after the
/// host is gone. The host makes it in its constructor, which puts it ahead of
/// everything the controller adopts from a module later, and QObject deletes
/// its children in the order they were added. So the backends go before those
/// controllers and domains do. The host holds it through a QPointer and
/// checks it before every use.
///
/// GUI thread only, like the host.
class ModuleConfigBackends final : public QObject
{
public:
    using Factory = std::function<std::unique_ptr<PhosphorConfig::IBackend>(const QString&)>;

    explicit ModuleConfigBackends(QObject* parent);

    /// Make every file a backend is created for under @p directory, an
    /// absolute path that is not the filesystem root (ModuleHost::
    /// setConfigRedirect checks it), instead of where it is. Empty redirects
    /// nothing.
    void setRedirect(const QString& directory);

    /// The backend shared for @p path, an absolute path, created with
    /// @p create the first time any module opens the file. Null when
    /// @p create made none, or this object was destroyed while @p create ran.
    PhosphorConfig::IBackend* shared(const QString& path, const Factory& create);
    /// The backend shared for @p path, an absolute path, or null when no
    /// module opened that file.
    [[nodiscard]] PhosphorConfig::IBackend* find(const QString& path) const;

private:
    /// Where config files are made instead of where they are, or empty.
    QString m_redirect;
    // By configFileKey. Shared, not unique: QHash values must be copyable.
    // A PhosphorConfig::JsonBackend writes its unsaved changes when it is
    // destroyed, which IBackend does not promise of other backends.
    QHash<QString, std::shared_ptr<PhosphorConfig::IBackend>> m_backends;
};

} // namespace PhosphorControl
