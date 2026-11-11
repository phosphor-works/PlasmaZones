// SPDX-FileCopyrightText: 2026 fuddlesworth
// SPDX-License-Identifier: LGPL-2.1-or-later
#pragma once

#include "phosphorcontrol_export.h"

#include <QAbstractListModel>
#include <QList>
#include <QPointer>
#include <QString>
#include <QVariantMap>
#include <QtQml/qqmlregistration.h>

namespace PhosphorControl {

class ApplicationController;

Q_MOC_INCLUDE("PhosphorControl/ApplicationController.h")

/// The sections in one slot of one page, as ApplicationController::
/// extensionsFor() lists them, for FacetSlot's Repeater.
///
/// It reads the list again whenever settingsApp, pageId or slot change, and
/// on extensionsChanged for its page. Rows are matched by extension id, so a
/// section that stays keeps its delegate, and its loaded QML, while others
/// come, go or change state.
class PHOSPHORCONTROL_EXPORT FacetSlotModel : public QAbstractListModel
{
    Q_OBJECT
    Q_PROPERTY(PhosphorControl::ApplicationController* settingsApp READ settingsApp WRITE setSettingsApp NOTIFY
                   settingsAppChanged)
    Q_PROPERTY(QString pageId READ pageId WRITE setPageId NOTIFY pageIdChanged)
    Q_PROPERTY(QString slot READ slotId WRITE setSlotId NOTIFY slotIdChanged)
    Q_PROPERTY(int count READ count NOTIFY countChanged)
    QML_ELEMENT

public:
    enum Role {
        ExtensionIdRole = Qt::UserRole + 1,
        ModuleIdRole,
        ModuleNameRole,
        NameRole,
        ModulePageIdRole,
        QmlSourceRole,
        DomainRole,
        FailedRole,
        ModuleFailedRole,
        DisabledAfterCrashRole,
        ErrorRole,
    };
    Q_ENUM(Role)

    explicit FacetSlotModel(QObject* parent = nullptr);

    ApplicationController* settingsApp() const;
    void setSettingsApp(ApplicationController* settingsApp);
    QString pageId() const;
    void setPageId(const QString& pageId);
    QString slotId() const;
    void setSlotId(const QString& slot);
    int count() const;

    int rowCount(const QModelIndex& parent = QModelIndex()) const override;
    QVariant data(const QModelIndex& index, int role) const override;
    QHash<int, QByteArray> roleNames() const override;

Q_SIGNALS:
    void settingsAppChanged();
    void pageIdChanged();
    void slotIdChanged();
    void countChanged();

private:
    /// One section: the entry extensionsFor() listed, and its domain held
    /// through a guard, since the domain can be destroyed before the host's
    /// notice of that reaches this model.
    struct Row
    {
        QVariantMap entry;
        QPointer<QObject> domain;
    };

    void refresh();
    void applyList(const QList<QVariantMap>& next);

    QPointer<ApplicationController> m_settingsApp;
    QString m_pageId;
    QString m_slot;
    QList<Row> m_rows;
    /// A section's QML is built while rows are inserted, and it can change
    /// what this model shows (a navigation moves pageId). Such a refresh is
    /// run once the one in progress is done.
    bool m_refreshing = false;
    bool m_refreshPending = false;
};

} // namespace PhosphorControl
