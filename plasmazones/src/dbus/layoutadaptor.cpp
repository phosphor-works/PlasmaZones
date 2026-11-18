// SPDX-FileCopyrightText: 2026 fuddlesworth
// SPDX-License-Identifier: GPL-3.0-or-later

#include "layoutadaptor.h"
#include "common/engineruntimenamespace.h"

#include "runtime/services/layoutservice/layoutservice.h"

namespace PlasmaZones {

LayoutAdaptor::LayoutAdaptor(LayoutService* service, QObject* parent)
    : QDBusAbstractAdaptor(parent)
    , m_service(service)
{
    Q_ASSERT(service);
    connect(service, &LayoutService::daemonReady, this, &LayoutAdaptor::daemonReady);
    connect(service, &LayoutService::layoutChanged, this, &LayoutAdaptor::layoutChanged);
    connect(service, &LayoutService::activeLayoutChanged, this, &LayoutAdaptor::activeLayoutChanged);
    connect(service, &LayoutService::layoutPropertyChanged, this,
            [this](const QString& layoutId, const QString& property, const QVariant& value) {
                Q_EMIT layoutPropertyChanged(layoutId, property, QDBusVariant(value));
            });
    connect(service, &LayoutService::layoutListChanged, this, &LayoutAdaptor::layoutListChanged);
    connect(service, &LayoutService::scrollingTemplatesChanged, this, &LayoutAdaptor::scrollingTemplatesChanged);
    connect(service, &LayoutService::layoutCreated, this, &LayoutAdaptor::layoutCreated);
    connect(service, &LayoutService::layoutDeleted, this, &LayoutAdaptor::layoutDeleted);
    connect(service, &LayoutService::screenLayoutChanged, this, &LayoutAdaptor::screenLayoutChanged);
    connect(service, &LayoutService::activeLayoutForScreenChanged, this, &LayoutAdaptor::activeLayoutForScreenChanged);
    connect(service, &LayoutService::virtualDesktopCountChanged, this, &LayoutAdaptor::virtualDesktopCountChanged);
    connect(service, &LayoutService::virtualDesktopNamesChanged, this, &LayoutAdaptor::virtualDesktopNamesChanged);
    connect(service, &LayoutService::activeLayoutIdChanged, this, &LayoutAdaptor::activeLayoutIdChanged);
    connect(service, &LayoutService::quickLayoutSlotsChanged, this, &LayoutAdaptor::quickLayoutSlotsChanged);
    connect(service, &LayoutService::currentActivityChanged, this, &LayoutAdaptor::currentActivityChanged);
    connect(service, &LayoutService::activitiesChanged, this, &LayoutAdaptor::activitiesChanged);
    connect(service, &LayoutService::assignmentChangesApplied, this, &LayoutAdaptor::assignmentChangesApplied);
    connect(service, &LayoutService::screenStatesChanged, this, &LayoutAdaptor::screenStatesChanged);
}

LayoutAdaptor::~LayoutAdaptor() = default;

QString LayoutAdaptor::getActiveLayout()
{
    return m_service ? m_service->getActiveLayout() : QString{};
}

QStringList LayoutAdaptor::getLayoutList()
{
    return m_service ? m_service->getLayoutList() : QStringList{};
}

QString LayoutAdaptor::getLayout(const QString& id)
{
    return m_service ? m_service->getLayout(id) : QString{};
}

QString LayoutAdaptor::getLayoutPreviewList()
{
    return m_service ? m_service->getLayoutPreviewList() : QString{};
}

QString LayoutAdaptor::getLayoutPreview(const QString& id, int windowCount)
{
    return m_service ? m_service->getLayoutPreview(id, windowCount) : QString{};
}

void LayoutAdaptor::setActiveLayout(const QString& id)
{
    if (m_service) {
        m_service->setActiveLayout(id);
    }
}

void LayoutAdaptor::applyQuickLayout(int mode, int number, const QString& screenId)
{
    if (m_service) {
        m_service->applyQuickLayout(mode, number, screenId);
    }
}

QString LayoutAdaptor::createLayout(const QString& name, const QString& type)
{
    return m_service ? m_service->createLayout(name, type) : QString{};
}

void LayoutAdaptor::deleteLayout(const QString& id)
{
    if (m_service) {
        m_service->deleteLayout(id);
    }
}

QString LayoutAdaptor::duplicateLayout(const QString& id)
{
    return m_service ? m_service->duplicateLayout(id) : QString{};
}

QString LayoutAdaptor::importLayout(const QString& filePath)
{
    return m_service ? m_service->importLayout(filePath) : QString{};
}

bool LayoutAdaptor::exportLayout(const QString& layoutId, const QString& filePath)
{
    return m_service ? m_service->exportLayout(layoutId, filePath) : bool{};
}

void LayoutAdaptor::setLayoutHidden(const QString& layoutId, bool hidden)
{
    if (m_service) {
        m_service->setLayoutHidden(layoutId, hidden);
    }
}

void LayoutAdaptor::setLayoutAutoAssign(const QString& layoutId, bool enabled)
{
    if (m_service) {
        m_service->setLayoutAutoAssign(layoutId, enabled);
    }
}

void LayoutAdaptor::setLayoutAspectRatioClass(const QString& layoutId, int aspectRatioClass)
{
    if (m_service) {
        m_service->setLayoutAspectRatioClass(layoutId, aspectRatioClass);
    }
}

bool LayoutAdaptor::updateLayout(const QString& layoutJson)
{
    return m_service ? m_service->updateLayout(layoutJson) : bool{};
}

QString LayoutAdaptor::createLayoutFromJson(const QString& layoutJson)
{
    return m_service ? m_service->createLayoutFromJson(layoutJson) : QString{};
}

bool LayoutAdaptor::openEditor()
{
    return m_service ? m_service->openEditor() : bool{};
}

bool LayoutAdaptor::openEditorForScreen(const QString& screenId)
{
    return m_service ? m_service->openEditorForScreen(screenId) : bool{};
}

bool LayoutAdaptor::openEditorForLayoutOnScreen(const QString& layoutId, const QString& screenId)
{
    return m_service ? m_service->openEditorForLayoutOnScreen(layoutId, screenId) : bool{};
}

bool LayoutAdaptor::openEditorForScrollingTemplate(const QString& templateId)
{
    return m_service ? m_service->openEditorForScrollingTemplate(templateId) : bool{};
}

QString LayoutAdaptor::getLayoutForScreen(const QString& screenId)
{
    return m_service ? m_service->getLayoutForScreen(screenId) : QString{};
}

QString LayoutAdaptor::getAssignedLayoutForScreen(const QString& screenId)
{
    return m_service ? m_service->getAssignedLayoutForScreen(screenId) : QString{};
}

void LayoutAdaptor::assignLayoutToScreen(const QString& screenId, const QString& layoutId)
{
    if (m_service) {
        m_service->assignLayoutToScreen(screenId, layoutId);
    }
}

void LayoutAdaptor::clearAssignment(const QString& screenId)
{
    if (m_service) {
        m_service->clearAssignment(screenId);
    }
}

void LayoutAdaptor::setAllScreenAssignments(const QVariantMap& assignments)
{
    if (m_service) {
        m_service->setAllScreenAssignments(assignments);
    }
}

QString LayoutAdaptor::getLayoutForScreenDesktop(const QString& screenId, int virtualDesktop)
{
    return m_service ? m_service->getLayoutForScreenDesktop(screenId, virtualDesktop) : QString{};
}

void LayoutAdaptor::assignLayoutToScreenDesktop(const QString& screenId, int virtualDesktop, const QString& layoutId)
{
    if (m_service) {
        m_service->assignLayoutToScreenDesktop(screenId, virtualDesktop, layoutId);
    }
}

void LayoutAdaptor::clearAssignmentForScreenDesktop(const QString& screenId, int virtualDesktop)
{
    if (m_service) {
        m_service->clearAssignmentForScreenDesktop(screenId, virtualDesktop);
    }
}

bool LayoutAdaptor::hasExplicitAssignmentForScreenDesktop(const QString& screenId, int virtualDesktop)
{
    return m_service ? m_service->hasExplicitAssignmentForScreenDesktop(screenId, virtualDesktop) : bool{};
}

int LayoutAdaptor::getModeForScreenDesktop(const QString& screenId, int virtualDesktop)
{
    return m_service ? m_service->getModeForScreenDesktop(screenId, virtualDesktop) : int{};
}

QString LayoutAdaptor::getSnappingLayoutForScreenDesktop(const QString& screenId, int virtualDesktop)
{
    return m_service ? m_service->getSnappingLayoutForScreenDesktop(screenId, virtualDesktop) : QString{};
}

QString LayoutAdaptor::getTilingAlgorithmForScreenDesktop(const QString& screenId, int virtualDesktop)
{
    return m_service ? m_service->getTilingAlgorithmForScreenDesktop(screenId, virtualDesktop) : QString{};
}

void LayoutAdaptor::setAllDesktopAssignments(const QVariantMap& assignments)
{
    if (m_service) {
        m_service->setAllDesktopAssignments(assignments);
    }
}

void LayoutAdaptor::setAssignmentEntry(const QString& screenId, int virtualDesktop, const QString& activity, int mode,
                                       const QString& snappingLayout, const QString& tilingAlgorithm)
{
    if (m_service) {
        m_service->setAssignmentEntry(screenId, virtualDesktop, activity, mode, snappingLayout, tilingAlgorithm);
    }
}

void LayoutAdaptor::setSaveBatchMode(bool enabled)
{
    if (m_service) {
        m_service->setSaveBatchMode(enabled);
    }
}

void LayoutAdaptor::applyAssignmentChanges()
{
    if (m_service) {
        m_service->applyAssignmentChanges();
    }
}

int LayoutAdaptor::getVirtualDesktopCount()
{
    return m_service ? m_service->getVirtualDesktopCount() : int{};
}

QStringList LayoutAdaptor::getVirtualDesktopNames()
{
    return m_service ? m_service->getVirtualDesktopNames() : QStringList{};
}

QStringList LayoutAdaptor::getAvailableScreenIds()
{
    return m_service ? m_service->getAvailableScreenIds() : QStringList{};
}

QString LayoutAdaptor::getAllScreenAssignments()
{
    return m_service ? m_service->getAllScreenAssignments() : QString{};
}

QVariantMap LayoutAdaptor::getActiveLayoutsForScreens()
{
    return m_service ? m_service->getActiveLayoutsForScreens() : QVariantMap{};
}

QVariantMap LayoutAdaptor::getAllDesktopAssignments()
{
    return m_service ? m_service->getAllDesktopAssignments() : QVariantMap{};
}

QVariantMap LayoutAdaptor::getAllActivityAssignments()
{
    return m_service ? m_service->getAllActivityAssignments() : QVariantMap{};
}

QVariantMap LayoutAdaptor::getAllCombinedAssignments()
{
    return m_service ? m_service->getAllCombinedAssignments() : QVariantMap{};
}

QString LayoutAdaptor::getQuickLayoutSlot(int mode, int slotNumber)
{
    return m_service ? m_service->getQuickLayoutSlot(mode, slotNumber) : QString{};
}

void LayoutAdaptor::setQuickLayoutSlot(int mode, int slotNumber, const QString& layoutId)
{
    if (m_service) {
        m_service->setQuickLayoutSlot(mode, slotNumber, layoutId);
    }
}

void LayoutAdaptor::setAllQuickLayoutSlots(int mode, const QVariantMap& slots)
{
    if (m_service) {
        m_service->setAllQuickLayoutSlots(mode, slots);
    }
}

QVariantMap LayoutAdaptor::getAllQuickLayoutSlots(int mode)
{
    return m_service ? m_service->getAllQuickLayoutSlots(mode) : QVariantMap{};
}

int LayoutAdaptor::getCurrentVirtualDesktop()
{
    return m_service ? m_service->getCurrentVirtualDesktop() : int{};
}

bool LayoutAdaptor::isActivitiesAvailable()
{
    return m_service ? m_service->isActivitiesAvailable() : bool{};
}

QStringList LayoutAdaptor::getActivities()
{
    return m_service ? m_service->getActivities() : QStringList{};
}

QString LayoutAdaptor::getCurrentActivity()
{
    return m_service ? m_service->getCurrentActivity() : QString{};
}

QString LayoutAdaptor::getActivityInfo(const QString& activityId)
{
    return m_service ? m_service->getActivityInfo(activityId) : QString{};
}

QString LayoutAdaptor::getAllActivitiesInfo()
{
    return m_service ? m_service->getAllActivitiesInfo() : QString{};
}

QString LayoutAdaptor::getLayoutForScreenActivity(const QString& screenId, const QString& activityId)
{
    return m_service ? m_service->getLayoutForScreenActivity(screenId, activityId) : QString{};
}

void LayoutAdaptor::assignLayoutToScreenActivity(const QString& screenId, const QString& activityId,
                                                 const QString& layoutId)
{
    if (m_service) {
        m_service->assignLayoutToScreenActivity(screenId, activityId, layoutId);
    }
}

void LayoutAdaptor::clearAssignmentForScreenActivity(const QString& screenId, const QString& activityId)
{
    if (m_service) {
        m_service->clearAssignmentForScreenActivity(screenId, activityId);
    }
}

bool LayoutAdaptor::hasExplicitAssignmentForScreenActivity(const QString& screenId, const QString& activityId)
{
    return m_service ? m_service->hasExplicitAssignmentForScreenActivity(screenId, activityId) : bool{};
}

void LayoutAdaptor::setAllActivityAssignments(const QVariantMap& assignments)
{
    if (m_service) {
        m_service->setAllActivityAssignments(assignments);
    }
}

void LayoutAdaptor::setAllCombinedAssignments(const QVariantMap& assignments)
{
    if (m_service) {
        m_service->setAllCombinedAssignments(assignments);
    }
}

QString LayoutAdaptor::getLayoutForScreenDesktopActivity(const QString& screenId, int virtualDesktop,
                                                         const QString& activityId)
{
    return m_service ? m_service->getLayoutForScreenDesktopActivity(screenId, virtualDesktop, activityId) : QString{};
}

void LayoutAdaptor::assignLayoutToScreenDesktopActivity(const QString& screenId, int virtualDesktop,
                                                        const QString& activityId, const QString& layoutId)
{
    if (m_service) {
        m_service->assignLayoutToScreenDesktopActivity(screenId, virtualDesktop, activityId, layoutId);
    }
}

void LayoutAdaptor::clearAssignmentForScreenDesktopActivity(const QString& screenId, int virtualDesktop,
                                                            const QString& activityId)
{
    if (m_service) {
        m_service->clearAssignmentForScreenDesktopActivity(screenId, virtualDesktop, activityId);
    }
}

void LayoutAdaptor::setScrollingTemplateLayout(const QString& screenId, int virtualDesktop, const QString& activityId,
                                               const QString& layoutId)
{
    if (m_service) {
        m_service->setScrollingTemplateLayout(screenId, virtualDesktop, activityId, layoutId);
    }
}

QString LayoutAdaptor::getScrollingTemplateLayout(const QString& screenId, int virtualDesktop,
                                                  const QString& activityId)
{
    return m_service ? m_service->getScrollingTemplateLayout(screenId, virtualDesktop, activityId) : QString{};
}

QString LayoutAdaptor::getScrollingTemplates()
{
    return m_service ? m_service->getScrollingTemplates() : QString{};
}

QString LayoutAdaptor::saveScrollingTemplate(const QString& templateJson)
{
    return m_service ? m_service->saveScrollingTemplate(templateJson) : QString{};
}

bool LayoutAdaptor::deleteScrollingTemplate(const QString& id)
{
    return m_service ? m_service->deleteScrollingTemplate(id) : bool{};
}

QString LayoutAdaptor::duplicateScrollingTemplate(const QString& id)
{
    return m_service ? m_service->duplicateScrollingTemplate(id) : QString{};
}

QString LayoutAdaptor::getScreenStates()
{
    return m_service ? m_service->getScreenStates() : QString{};
}

} // namespace PlasmaZones
