// SPDX-FileCopyrightText: 2026 fuddlesworth
// SPDX-License-Identifier: LGPL-2.1-or-later
#pragma once

#include "PhosphorControl/PageController.h"
#include "phosphorcontrol_export.h"

#include <QString>
#include <QtQml/qqmlregistration.h>

namespace PhosphorControl {

/**
 * The controller of a page that stands in for a Facet module's page while
 * the module is not loaded. FacetHost registers one per placeholder, and
 * FacetModulePage.qml, the page it shows (FacetHost::placeholderPageSource),
 * declares its `controller` as this type, so QML checks the members it reads.
 *
 * Never dirty: apply() and discard() succeed at once, so a placeholder never
 * holds up a batch. The module's state comes from the implementation, which
 * emits moduleStateChanged() whenever any of failed, disabledAfterCrash,
 * reason or canRetry may have changed.
 */
class PHOSPHORCONTROL_EXPORT ModulePlaceholderPage : public PageController
{
    Q_OBJECT
    /// The module's display name.
    Q_PROPERTY(QString moduleName READ moduleName CONSTANT)
    /// The module failed: its library, its metadata or its registration.
    Q_PROPERTY(bool failed READ failed NOTIFY moduleStateChanged)
    /// The module was turned off because Facet stopped while it loaded.
    Q_PROPERTY(bool disabledAfterCrash READ disabledAfterCrash NOTIFY moduleStateChanged)
    /// The host's diagnostic account of a failure. Not translated.
    Q_PROPERTY(QString reason READ reason NOTIFY moduleStateChanged)
    /// Whether retry() can load the module now.
    Q_PROPERTY(bool canRetry READ canRetry NOTIFY moduleStateChanged)
    QML_NAMED_ELEMENT(ModulePlaceholderPage)
    QML_UNCREATABLE("ModulePlaceholderPage is an abstract base; FacetHost creates the placeholders.")

public:
    explicit ModulePlaceholderPage(const QString& id, QObject* parent = nullptr);
    ~ModulePlaceholderPage() override;

    bool isDirty() const override;
    void apply() override;
    void discard() override;

    virtual QString moduleName() const = 0;
    virtual bool failed() const = 0;
    virtual bool disabledAfterCrash() const = 0;
    virtual QString reason() const = 0;
    virtual bool canRetry() const = 0;

    /// Load the module again, or for the first time when it is still
    /// available.
    Q_INVOKABLE virtual void retry() = 0;

Q_SIGNALS:
    void moduleStateChanged();
};

} // namespace PhosphorControl
