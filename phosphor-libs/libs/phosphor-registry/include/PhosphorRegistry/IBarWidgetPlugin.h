// SPDX-FileCopyrightText: 2026 fuddlesworth
// SPDX-License-Identifier: LGPL-2.1-or-later
#pragma once

#include <PhosphorRegistry/IBarWidgetFactory.h>
#include <PhosphorRegistry/phosphorregistry_export.h>

#include <QtCore/qtclasshelpermacros.h>
#include <QtPlugin>

#include <memory>

namespace PhosphorRegistry {

/// The root object of a bar-widget plugin.
///
/// A bar-widget plugin is a Qt plugin in the `bar-widget` seam (see
/// PluginLoader::barWidgetSeam): its root QObject implements this interface,
/// declares `Q_PLUGIN_METADATA(IID PhosphorRegistryBarWidgetPlugin_iid FILE
/// "metadata.json")`, and its metadata carries `"Id"` and `"ApiVersion"`.
///
/// The plugin and the host must use the same C++ standard library, because
/// the factory crosses this interface in a std::unique_ptr that the host
/// adopts into a std::shared_ptr.
class PHOSPHORREGISTRY_EXPORT IBarWidgetPlugin
{
public:
    virtual ~IBarWidgetPlugin() = default;
    Q_DISABLE_COPY_MOVE(IBarWidgetPlugin)

    /// A fresh factory whose id() equals the plugin's metadata `"Id"`, or
    /// nullptr when it cannot be built. Called each time the plugin comes on
    /// offer, which happens more than once when its file is removed and put
    /// back. Must not throw: the library is built without exceptions, so an
    /// exception that escapes ends the process.
    [[nodiscard]] virtual std::unique_ptr<IBarWidgetFactory> createFactory() = 0;

protected:
    IBarWidgetPlugin() = default;
};

} // namespace PhosphorRegistry

#define PhosphorRegistryBarWidgetPlugin_iid "org.phosphor_works.Lambent.BarWidget/1"
Q_DECLARE_INTERFACE(PhosphorRegistry::IBarWidgetPlugin, PhosphorRegistryBarWidgetPlugin_iid)
