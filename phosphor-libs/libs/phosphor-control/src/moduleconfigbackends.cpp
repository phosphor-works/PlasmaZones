// SPDX-FileCopyrightText: 2026 fuddlesworth
// SPDX-License-Identifier: LGPL-2.1-or-later

#include "moduleconfigbackends_p.h"

#include <PhosphorConfig/IBackend.h>

#include <QDir>
#include <QFileInfo>
#include <QPointer>
#include <QStringList>

#include <utility>

namespace PhosphorControl {

namespace {
// The file the OS opens for @p path, which ModuleContext::configBackend and
// ModuleHost::configBackend have checked is absolute: symbolic links followed,
// and `.`, `..` and doubled separators removed. The file and the directories
// above it may not exist yet, so the walk goes one name at a time and keeps
// whatever is missing as written, where QFileInfo::canonicalFilePath gives
// up. The key is therefore the same before the directory exists, once it
// does and once the file does. A `..` leaves the directory a link leads to,
// as it does for the OS, where QDir::cleanPath drops the link's name
// instead. A link whose target is missing is followed too, because writing
// through it creates that target.
QString configFileKey(const QString& path)
{
    // What Linux follows in one lookup. A cycle of links ends here too, with
    // the name it stopped at kept as written.
    constexpr int MaxLinksFollowed = 40;
    const auto namesIn = [](const QString& part) {
        return part.split(QLatin1Char('/'), Qt::SkipEmptyParts);
    };
    QStringList pending = namesIn(path);
    QStringList resolved;
    int linksFollowed = 0;
    while (!pending.isEmpty()) {
        const QString name = pending.takeFirst();
        if (name == QLatin1String(".")) {
            continue;
        }
        if (name == QLatin1String("..")) {
            if (!resolved.isEmpty()) {
                resolved.removeLast();
            }
            continue;
        }
        const QFileInfo entry(QLatin1Char('/') + (resolved + QStringList{name}).join(QLatin1Char('/')));
        const QString target =
            entry.isSymbolicLink() && linksFollowed < MaxLinksFollowed ? entry.readSymLink() : QString();
        if (target.isEmpty()) {
            resolved.append(name);
            continue;
        }
        ++linksFollowed;
        // A relative target starts in the link's own directory, which is
        // what `resolved` holds.
        if (target.startsWith(QLatin1Char('/'))) {
            resolved.clear();
        }
        pending = namesIn(target) + pending;
    }
    return QLatin1Char('/') + resolved.join(QLatin1Char('/'));
}
} // namespace

ModuleConfigBackends::ModuleConfigBackends(QObject* parent)
    : QObject(parent)
{
}

void ModuleConfigBackends::setRedirect(const QString& directory)
{
    m_redirect = directory;
}

PhosphorConfig::IBackend* ModuleConfigBackends::shared(const QString& path, const Factory& create)
{
    const QString key = configFileKey(path);
    const auto existing = m_backends.constFind(key);
    if (existing != m_backends.cend()) {
        return existing->get();
    }
    // The backends stay keyed by the file the module named. A redirect
    // (setRedirect) only changes where that file's backend is made: the same
    // path again, under the redirect directory, which is created because the
    // module could not know to.
    QString file = key;
    if (!m_redirect.isEmpty()) {
        file = QDir::cleanPath(m_redirect + QLatin1Char('/') + key);
        QDir().mkpath(QFileInfo(file).absolutePath());
    }
    const QPointer<ModuleConfigBackends> self(this);
    std::unique_ptr<PhosphorConfig::IBackend> created = create ? create(file) : nullptr;
    // create is module code, so look again before using what it can have
    // changed. It can have destroyed the ApplicationController, and this
    // object with it.
    if (!self) {
        return nullptr;
    }
    // It can have opened this same file through the host. The backend that
    // call shared is already in use, so it stands, and the one made here is
    // dropped rather than put in its place. That holds when create returned
    // null too: the file's shared backend is the answer whoever made it.
    if (const auto nested = m_backends.constFind(key); nested != m_backends.cend()) {
        return nested->get();
    }
    if (!created) {
        return nullptr;
    }
    PhosphorConfig::IBackend* const backend = created.get();
    m_backends.insert(key, std::shared_ptr<PhosphorConfig::IBackend>(std::move(created)));
    return backend;
}

PhosphorConfig::IBackend* ModuleConfigBackends::find(const QString& path) const
{
    const auto existing = m_backends.constFind(configFileKey(path));
    return existing != m_backends.cend() ? existing->get() : nullptr;
}

} // namespace PhosphorControl
