// SPDX-FileCopyrightText: 2026 fuddlesworth
// SPDX-License-Identifier: GPL-3.0-or-later

#pragma once

#include "plasmazones_export.h"

#include <QObject>
#include <QString>
#include <QStringList>

namespace PlasmaZones {

/// In-process notices of a window's close and of a prune sweep, for the
/// sibling adaptors that keep their own per-window state, and of the effect's
/// per-output-desktops mode and per-screen desktop reports, for the
/// dynamic-workspaces feature. A plain
/// QObject, not an adaptor signal, so none of these reaches the session bus.
class PLASMAZONES_EXPORT WindowLifecycleRelay : public QObject
{
    Q_OBJECT

public:
    using QObject::QObject;

Q_SIGNALS:
    /// Emitted once by WindowTrackingAdaptor::windowClosed after its teardown.
    void windowClosed(const QString& windowId);
    /// The instance ids alive at a WindowTrackingAdaptor::pruneStaleWindows sweep.
    void stalePruned(const QStringList& aliveInstances);
    /// The effect's per-output-desktops report moved
    /// (WindowTrackingAdaptor::reportPerOutputDesktopsMode). Change-gated: a
    /// subscriber that connects after the effect reported hears nothing and
    /// reads WindowTrackingAdaptor::perOutputDesktopsMode() for the answer.
    void perOutputDesktopsModeReported(bool enabled);
    /// Every admitted WindowTrackingAdaptor::screenDesktopChanged report, changed
    /// or not, emitted before VirtualDesktopManager::updateScreenDesktop applies
    /// it. The manager emits only on a real change, so a report that repeats its
    /// row (the effect's forced answer to an already satisfied switch request)
    /// reaches a subscriber only through this signal.
    void screenDesktopReported(const QString& screenId, int desktop);
};

} // namespace PlasmaZones
