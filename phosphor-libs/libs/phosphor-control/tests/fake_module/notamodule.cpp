// SPDX-FileCopyrightText: 2026 fuddlesworth
// SPDX-License-Identifier: LGPL-2.1-or-later

// A plugin that carries the control module IID and valid metadata but whose root
// object does not implement IControlModule, so the catalog accepts it and
// ModuleHost::loadModule must refuse it.

#include <PhosphorControl/ControlModule.h>

#include <QObject>

class NotAControlModule : public QObject
{
    Q_OBJECT
    Q_PLUGIN_METADATA(IID PhosphorControlModule_iid FILE "module_metadata.json")
};

#include "notamodule.moc"
