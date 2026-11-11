// SPDX-FileCopyrightText: 2026 fuddlesworth
// SPDX-License-Identifier: LGPL-2.1-or-later

#include "PhosphorControl/FacetSlotModel.h"

#include "PhosphorControl/ApplicationController.h"

#include <QSet>
#include <QUrl>

namespace PhosphorControl {

namespace {

QString entryId(const QVariantMap& entry)
{
    return entry.value(QStringLiteral("id")).toString();
}

QObject* entryDomain(const QVariantMap& entry)
{
    return entry.value(QStringLiteral("domain")).value<QObject*>();
}

} // namespace

FacetSlotModel::FacetSlotModel(QObject* parent)
    : QAbstractListModel(parent)
{
}

ApplicationController* FacetSlotModel::settingsApp() const
{
    return m_settingsApp.data();
}

void FacetSlotModel::setSettingsApp(ApplicationController* settingsApp)
{
    if (m_settingsApp == settingsApp) {
        return;
    }
    if (m_settingsApp) {
        disconnect(m_settingsApp, nullptr, this, nullptr);
    }
    m_settingsApp = settingsApp;
    if (m_settingsApp) {
        connect(m_settingsApp, &ApplicationController::extensionsChanged, this, [this](const QString& pageId) {
            if (pageId == m_pageId) {
                refresh();
            }
        });
        connect(m_settingsApp, &QObject::destroyed, this, [this]() {
            m_settingsApp = nullptr;
            refresh();
            Q_EMIT settingsAppChanged();
        });
    }
    Q_EMIT settingsAppChanged();
    refresh();
}

QString FacetSlotModel::pageId() const
{
    return m_pageId;
}

void FacetSlotModel::setPageId(const QString& pageId)
{
    if (m_pageId == pageId) {
        return;
    }
    m_pageId = pageId;
    Q_EMIT pageIdChanged();
    refresh();
}

QString FacetSlotModel::slotId() const
{
    return m_slot;
}

void FacetSlotModel::setSlotId(const QString& slot)
{
    if (m_slot == slot) {
        return;
    }
    m_slot = slot;
    Q_EMIT slotIdChanged();
    refresh();
}

int FacetSlotModel::count() const
{
    return static_cast<int>(m_rows.size());
}

int FacetSlotModel::rowCount(const QModelIndex& parent) const
{
    return parent.isValid() ? 0 : count();
}

QVariant FacetSlotModel::data(const QModelIndex& index, int role) const
{
    if (!checkIndex(index, CheckIndexOption::IndexIsValid | CheckIndexOption::ParentIsInvalid)) {
        return QVariant();
    }
    const Row& row = m_rows.at(index.row());
    const QVariantMap& entry = row.entry;
    switch (role) {
    case ExtensionIdRole:
        return entryId(entry);
    case ModuleIdRole:
        return entry.value(QStringLiteral("moduleId")).toString();
    case ModuleNameRole:
        return entry.value(QStringLiteral("moduleName")).toString();
    case NameRole:
        return entry.value(QStringLiteral("name")).toString();
    case ModulePageIdRole:
        return entry.value(QStringLiteral("modulePageId")).toString();
    case QmlSourceRole:
        return entry.value(QStringLiteral("qmlSource")).toUrl();
    case DomainRole:
        return QVariant::fromValue<QObject*>(row.domain.data());
    case FailedRole:
        return entry.contains(QStringLiteral("error"));
    case ModuleFailedRole:
        return entry.value(QStringLiteral("moduleFailed")).toBool();
    case DisabledAfterCrashRole:
        return entry.value(QStringLiteral("disabledAfterCrash")).toBool();
    case ErrorRole:
        return entry.value(QStringLiteral("error")).toString();
    default:
        return QVariant();
    }
}

QHash<int, QByteArray> FacetSlotModel::roleNames() const
{
    return {
        {ExtensionIdRole, QByteArrayLiteral("extensionId")},
        {ModuleIdRole, QByteArrayLiteral("moduleId")},
        {ModuleNameRole, QByteArrayLiteral("moduleName")},
        {NameRole, QByteArrayLiteral("name")},
        {ModulePageIdRole, QByteArrayLiteral("modulePageId")},
        {QmlSourceRole, QByteArrayLiteral("qmlSource")},
        {DomainRole, QByteArrayLiteral("domain")},
        {FailedRole, QByteArrayLiteral("failed")},
        {ModuleFailedRole, QByteArrayLiteral("moduleFailed")},
        {DisabledAfterCrashRole, QByteArrayLiteral("disabledAfterCrash")},
        {ErrorRole, QByteArrayLiteral("error")},
    };
}

void FacetSlotModel::refresh()
{
    if (m_refreshing) {
        m_refreshPending = true;
        return;
    }
    m_refreshing = true;
    do {
        m_refreshPending = false;
        QList<QVariantMap> next;
        if (m_settingsApp && !m_pageId.isEmpty()) {
            const QVariantList listed = m_settingsApp->extensionsFor(m_pageId, m_slot);
            for (const QVariant& entry : listed) {
                next.append(entry.toMap());
            }
        }
        applyList(next);
    } while (m_refreshPending);
    m_refreshing = false;
}

void FacetSlotModel::applyList(const QList<QVariantMap>& next)
{
    const int countBefore = count();

    // Rows whose section is gone.
    QSet<QString> nextIds;
    for (const QVariantMap& entry : std::as_const(next)) {
        nextIds.insert(entryId(entry));
    }
    for (qsizetype row = m_rows.size() - 1; row >= 0; --row) {
        if (!nextIds.contains(entryId(m_rows.at(row).entry))) {
            beginRemoveRows(QModelIndex(), static_cast<int>(row), static_cast<int>(row));
            m_rows.removeAt(row);
            endRemoveRows();
        }
    }
    // Then walk the new order: keep a row that is in place, move one that is
    // further down, insert one that is new. A kept row keeps its delegate.
    for (qsizetype row = 0; row < next.size(); ++row) {
        const QVariantMap& wanted = next.at(row);
        const QString id = entryId(wanted);
        qsizetype found = -1;
        for (qsizetype candidate = row; candidate < m_rows.size(); ++candidate) {
            if (entryId(m_rows.at(candidate).entry) == id) {
                found = candidate;
                break;
            }
        }
        if (found < 0) {
            beginInsertRows(QModelIndex(), static_cast<int>(row), static_cast<int>(row));
            m_rows.insert(row, Row{wanted, entryDomain(wanted)});
            endInsertRows();
            continue;
        }
        if (found != row) {
            beginMoveRows(QModelIndex(), static_cast<int>(found), static_cast<int>(found), QModelIndex(),
                          static_cast<int>(row));
            m_rows.move(found, row);
            endMoveRows();
        }
        if (m_rows.at(row).entry != wanted) {
            m_rows[row] = Row{wanted, entryDomain(wanted)};
            const QModelIndex changed = index(static_cast<int>(row));
            Q_EMIT dataChanged(changed, changed);
        }
    }
    // Rows left over past the end: an id listed twice before and once now.
    if (m_rows.size() > next.size()) {
        beginRemoveRows(QModelIndex(), static_cast<int>(next.size()), static_cast<int>(m_rows.size() - 1));
        m_rows.resize(next.size());
        endRemoveRows();
    }
    if (count() != countBefore) {
        Q_EMIT countChanged();
    }
}

} // namespace PhosphorControl
