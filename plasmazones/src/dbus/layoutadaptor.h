// SPDX-FileCopyrightText: 2026 fuddlesworth
// SPDX-License-Identifier: GPL-3.0-or-later

#pragma once

#include "plasmazones_dbus_export.h"

#include <QDBusAbstractAdaptor>
#include <QDBusVariant>
#include <QJsonObject>
#include <QPointer>
#include <QString>
#include <QStringList>
#include <QVariantList>
#include <QVariantMap>

namespace PhosphorEngineRuntime {
class LayoutService;
} // namespace PhosphorEngineRuntime

namespace PlasmaZones {

/**
 * @brief The D-Bus binding of LayoutService: org.phosphor_works.Placement.LayoutRegistry
 *
 * Every method, property and signal here is one the interface XML names, with
 * the signature it pins; each one's behaviour is documented on the
 * LayoutService member of the same name. The binding forwards and relays
 * the service's signals.
 *
 * layoutPropertyChanged carries its value as a QDBusVariant on the bus, wrapped here.
 */
class PLASMAZONES_DBUS_EXPORT LayoutAdaptor : public QDBusAbstractAdaptor
{
    Q_OBJECT
    Q_CLASSINFO("D-Bus Interface", "org.phosphor_works.Placement.LayoutRegistry")

public:
    /// @param service Not owned. A call that arrives after it is gone is answered empty.
    /// @param parent The object the adaptor is exported through
    LayoutAdaptor(LayoutService* service, QObject* parent);
    ~LayoutAdaptor() override;

public Q_SLOTS:
    QString getActiveLayout();
    QStringList getLayoutList();
    QString getLayout(const QString& id);
    QString getLayoutPreviewList();
    QString getLayoutPreview(const QString& id, int windowCount);
    void setActiveLayout(const QString& id);
    void applyQuickLayout(int mode, int number, const QString& screenId);
    QString createLayout(const QString& name, const QString& type);
    void deleteLayout(const QString& id);
    QString duplicateLayout(const QString& id);
    QString importLayout(const QString& filePath);
    bool exportLayout(const QString& layoutId, const QString& filePath);
    void setLayoutHidden(const QString& layoutId, bool hidden);
    void setLayoutAutoAssign(const QString& layoutId, bool enabled);
    void setLayoutAspectRatioClass(const QString& layoutId, int aspectRatioClass);
    bool updateLayout(const QString& layoutJson);
    QString createLayoutFromJson(const QString& layoutJson);
    bool openEditor();
    bool openEditorForScreen(const QString& screenId);
    bool openEditorForLayoutOnScreen(const QString& layoutId, const QString& screenId);
    bool openEditorForScrollingTemplate(const QString& templateId);
    QString getLayoutForScreen(const QString& screenId);
    QString getAssignedLayoutForScreen(const QString& screenId);
    void assignLayoutToScreen(const QString& screenId, const QString& layoutId);
    void clearAssignment(const QString& screenId);
    void setAllScreenAssignments(const QVariantMap& assignments);
    QString getLayoutForScreenDesktop(const QString& screenId, int virtualDesktop);
    void assignLayoutToScreenDesktop(const QString& screenId, int virtualDesktop, const QString& layoutId);
    void clearAssignmentForScreenDesktop(const QString& screenId, int virtualDesktop);
    bool hasExplicitAssignmentForScreenDesktop(const QString& screenId, int virtualDesktop);
    int getModeForScreenDesktop(const QString& screenId, int virtualDesktop);
    QString getSnappingLayoutForScreenDesktop(const QString& screenId, int virtualDesktop);
    QString getTilingAlgorithmForScreenDesktop(const QString& screenId, int virtualDesktop);
    void setAllDesktopAssignments(const QVariantMap& assignments);
    void setAssignmentEntry(const QString& screenId, int virtualDesktop, const QString& activity, int mode,
                            const QString& snappingLayout, const QString& tilingAlgorithm);
    void setSaveBatchMode(bool enabled);
    void applyAssignmentChanges();
    int getVirtualDesktopCount();
    QStringList getVirtualDesktopNames();
    QStringList getAvailableScreenIds();
    QString getAllScreenAssignments();
    QVariantMap getActiveLayoutsForScreens();
    QVariantMap getAllDesktopAssignments();
    QVariantMap getAllActivityAssignments();
    QVariantMap getAllCombinedAssignments();
    QString getQuickLayoutSlot(int mode, int slotNumber);
    void setQuickLayoutSlot(int mode, int slotNumber, const QString& layoutId);
    void setAllQuickLayoutSlots(int mode, const QVariantMap& slots);
    QVariantMap getAllQuickLayoutSlots(int mode);
    int getCurrentVirtualDesktop();
    bool isActivitiesAvailable();
    QStringList getActivities();
    QString getCurrentActivity();
    QString getActivityInfo(const QString& activityId);
    QString getAllActivitiesInfo();
    QString getLayoutForScreenActivity(const QString& screenId, const QString& activityId);
    void assignLayoutToScreenActivity(const QString& screenId, const QString& activityId, const QString& layoutId);
    void clearAssignmentForScreenActivity(const QString& screenId, const QString& activityId);
    bool hasExplicitAssignmentForScreenActivity(const QString& screenId, const QString& activityId);
    void setAllActivityAssignments(const QVariantMap& assignments);
    void setAllCombinedAssignments(const QVariantMap& assignments);
    QString getLayoutForScreenDesktopActivity(const QString& screenId, int virtualDesktop, const QString& activityId);
    void assignLayoutToScreenDesktopActivity(const QString& screenId, int virtualDesktop, const QString& activityId,
                                             const QString& layoutId);
    void clearAssignmentForScreenDesktopActivity(const QString& screenId, int virtualDesktop,
                                                 const QString& activityId);
    void setScrollingTemplateLayout(const QString& screenId, int virtualDesktop, const QString& activityId,
                                    const QString& layoutId);
    QString getScrollingTemplateLayout(const QString& screenId, int virtualDesktop, const QString& activityId);
    QString getScrollingTemplates();
    QString saveScrollingTemplate(const QString& templateJson);
    bool deleteScrollingTemplate(const QString& id);
    QString duplicateScrollingTemplate(const QString& id);
    QString getScreenStates();

Q_SIGNALS:
    void daemonReady();
    void layoutChanged(const QString& layoutJson);
    void activeLayoutChanged(const QString& layoutId);
    void layoutPropertyChanged(const QString& layoutId, const QString& property, const QDBusVariant& value);
    void layoutListChanged();
    void scrollingTemplatesChanged();
    void layoutCreated(const QString& layoutId);
    void layoutDeleted(const QString& layoutId);
    void screenLayoutChanged(const QString& screenId, const QString& layoutId, int virtualDesktop);
    void activeLayoutForScreenChanged(const QString& screenId, const QString& layoutId);
    void virtualDesktopCountChanged(int count);
    void virtualDesktopNamesChanged(const QStringList& names);
    void activeLayoutIdChanged(const QString& layoutId);
    void quickLayoutSlotsChanged();
    void currentActivityChanged(const QString& activityId);
    void activitiesChanged();
    void assignmentChangesApplied(const QStringList& changedScreenIds);
    void screenStatesChanged();

private:
    QPointer<LayoutService> m_service;
};

} // namespace PlasmaZones
