// SPDX-FileCopyrightText: 2026 fuddlesworth
// SPDX-License-Identifier: LGPL-2.1-or-later

// A plugin that carries the Facet IID and valid metadata but whose root
// object does not implement IFacetModule, so the catalog accepts it and
// FacetHost::loadModule must refuse it.

#include <PhosphorControl/FacetModule.h>

#include <QObject>

class NotAFacetModule : public QObject
{
    Q_OBJECT
    Q_PLUGIN_METADATA(IID PhosphorFacetModule_iid FILE "metadata.json")
};

#include "notamodule.moc"
