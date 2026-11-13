// SPDX-FileCopyrightText: 2026 fuddlesworth
// SPDX-License-Identifier: LGPL-2.1-or-later

// One source for every bar-widget test plugin. tests/CMakeLists.txt builds
// it once per variant, each with its own fake_plugin_config.h and
// metadata.json in a per-variant include directory, so the variants differ
// only in their IID, their metadata and what their root object and factory
// do.

#include "fake_plugin_config.h"

#include <PhosphorRegistry/IBarWidgetPlugin.h>

#include <QObject>
#include <QQmlEngine>
#include <QQuickItem>
#include <QtCore/qtclasshelpermacros.h>

#include <memory>

#if FAKE_UNRESOLVED_SYMBOL
// Defined by no library. The variant is built with -fplt and links with
// -z undefs and -z lazy, so the reference survives into the plugin as a lazily
// bound call and only an eager bind at load time reports it.
extern "C" Q_DECL_IMPORT int phosphor_registry_test_missing_function();
#endif

namespace {

// A widget type whose code lives in the plugin, so using one after the
// library were unmapped would crash instead of passing.
class FakeWidget : public QQuickItem
{
    Q_OBJECT
public:
    explicit FakeWidget(QQuickItem* parent)
        : QQuickItem(parent)
    {
    }
};

class FakeFactory : public PhosphorRegistry::IBarWidgetFactory
{
public:
    FakeFactory() = default;
    ~FakeFactory() override = default;
    Q_DISABLE_COPY_MOVE(FakeFactory)

    [[nodiscard]] QString id() const override
    {
        return QStringLiteral(FAKE_FACTORY_ID);
    }
    [[nodiscard]] QString displayName() const override
    {
        return QStringLiteral("Fake Plugin");
    }
    [[nodiscard]] QStringList capabilities() const override
    {
        return {QStringLiteral("bar.widget")};
    }
    [[nodiscard]] QQuickItem* createWidget(QQmlEngine* /*engine*/, QObject* parent) override
    {
        auto* parentItem = qobject_cast<QQuickItem*>(parent);
        auto* item = new FakeWidget(parentItem);
        if (parent && !parentItem) {
            item->setParent(parent);
        }
        return item;
    }
};

} // namespace

#if FAKE_NO_INTERFACE
class FakeBarWidgetPlugin : public QObject
{
    Q_OBJECT
    Q_PLUGIN_METADATA(IID FAKE_PLUGIN_IID FILE "metadata.json")
};
#else
class FakeBarWidgetPlugin : public QObject, public PhosphorRegistry::IBarWidgetPlugin
{
    Q_OBJECT
    Q_PLUGIN_METADATA(IID FAKE_PLUGIN_IID FILE "metadata.json")
    Q_INTERFACES(PhosphorRegistry::IBarWidgetPlugin)

public:
    std::unique_ptr<PhosphorRegistry::IBarWidgetFactory> createFactory() override
    {
#if FAKE_UNRESOLVED_SYMBOL
        if (phosphor_registry_test_missing_function() != 0) {
            return nullptr;
        }
#endif
        if constexpr (FAKE_NULL_FACTORY) {
            return nullptr;
        }
        return std::make_unique<FakeFactory>();
    }
};
#endif

#include "fakeplugin.moc"
