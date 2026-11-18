// SPDX-FileCopyrightText: 2026 fuddlesworth
// SPDX-License-Identifier: GPL-3.0-or-later

#pragma once

#include "plasmazones_dbus_export.h"

#include <QDBusAbstractAdaptor>
#include <QJsonObject>
#include <QPointer>
#include <QString>
#include <QStringList>
#include <QVariantList>
#include <QVariantMap>

namespace PhosphorEngineRuntime {
class ScrollingService;
} // namespace PhosphorEngineRuntime

namespace PlasmaZones {

/**
 * @brief The D-Bus binding of ScrollingService: org.phosphor_works.Placement.Scrolling
 *
 * Every method, property and signal here is one the interface XML names, with
 * the signature it pins; each one's behaviour is documented on the
 * ScrollingService member of the same name. The binding forwards and relays
 * the service's signals.
 *
 * Its compositor-only reports (clearWindowedFullscreen, reapplyWindowGeometry, reportCommittedSize,
 * setWindowFullscreenFloat) admit only the registered bridge (DbusHelpers::admitCompositorReport, #1155).
 */
class PLASMAZONES_DBUS_EXPORT ScrollingAdaptor : public QDBusAbstractAdaptor
{
    Q_OBJECT
    Q_CLASSINFO("D-Bus Interface", "org.phosphor_works.Placement.Scrolling")
    Q_PROPERTY(QStringList scrollingScreens READ scrollingScreens NOTIFY scrollingScreensChanged)
    Q_PROPERTY(QVariantMap scrollEffectBehaviour READ scrollEffectBehaviour NOTIFY scrollEffectBehaviourChanged)
    Q_PROPERTY(QStringList scrollFocusScrollBlockedWindows READ scrollFocusScrollBlockedWindows NOTIFY
                   scrollFocusScrollBlockedWindowsChanged)

public:
    /// @param service Not owned. A call that arrives after it is gone is answered empty.
    /// @param parent The object the adaptor is exported through
    ScrollingAdaptor(ScrollingService* service, QObject* parent);
    ~ScrollingAdaptor() override;

    QStringList scrollingScreens() const;
    QVariantMap scrollEffectBehaviour() const;
    QStringList scrollFocusScrollBlockedWindows() const;

public Q_SLOTS:
    void focusColumn(const QString& screenId, int delta);
    void scrollView(const QString& screenId, int delta);
    void focusColumnAt(const QString& screenId, int index);
    void moveColumnTo(const QString& screenId, int from, int to);
    void scrollViewByPx(const QString& screenId, int px);
    void setColumnWidthProportion(const QString& screenId, double proportion);
    void setColumnWidthPixels(const QString& screenId, int px);
    void setWindowHeightProportion(const QString& screenId, double proportion);
    void setWindowHeightPixels(const QString& screenId, int px);
    bool toggleMaximizeColumn(const QString& screenId, const QString& windowId);
    bool toggleMaximizeToEdges(const QString& screenId, const QString& windowId);
    void clearWindowedFullscreen(const QString& windowId);
    void reapplyWindowGeometry(const QString& windowId);
    void reportCommittedSize(const QString& windowId, int offeredWidth, int offeredHeight, int committedWidth,
                             int committedHeight);
    bool setWindowFullscreenFloat(const QString& windowId, const QString& screenId, bool floating);
    QString visibleStripJson(const QString& screenId) const;
    QString stripModelJson(const QString& screenId) const;
    QString presetVocabularyJson(const QString& screenId) const;
    QString blueprintProgressJson(const QString& screenId) const;

Q_SIGNALS:
    void scrollingScreensChanged(const QStringList& screenIds, bool isContextSwitch,
                                 const QStringList& leavingToAutotile);
    void stripContextChanged(const QString& screenId, const QString& epoch, const QString& debugLabel);
    void scrollEffectBehaviourChanged(const QVariantMap& behaviour);
    void scrollFocusScrollBlockedWindowsChanged(const QStringList& windowIds);
    void stripChanged(const QString& screenId);

private:
    QPointer<ScrollingService> m_service;
};

} // namespace PlasmaZones
