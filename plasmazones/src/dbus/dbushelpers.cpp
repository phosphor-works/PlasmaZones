// SPDX-FileCopyrightText: 2026 fuddlesworth
// SPDX-License-Identifier: GPL-3.0-or-later

#include "dbushelpers.h"
#include "common/engineruntimenamespace.h"
#include "compositorbridgeadaptor.h"
#include "core/platform/logging.h"
#include <QDBusContext>
#include <QDBusMessage>
#include <QMetaClassInfo>
#include <QMetaObject>
#include <QSet>

namespace PlasmaZones {
namespace DbusHelpers {

const QDBusContext* busContext(const QObject* adaptor)
{
    QObject* const host = adaptor ? adaptor->parent() : nullptr;
    // The lookup Qt itself uses to find the context it sets.
    const auto* context = host ? static_cast<const QDBusContext*>(host->qt_metacast("QDBusContext")) : nullptr;
    return context && context->calledFromDBus() ? context : nullptr;
}

QString busCaller(const QObject* adaptor)
{
    const QDBusContext* const context = busContext(adaptor);
    return context ? context->message().service() : QString();
}

namespace {

/// Whether @p message is a method call addressed to @p adaptor's own D-Bus
/// interface, or one naming none, which Qt dispatches by member name.
bool isCallToOwnInterface(const QObject* adaptor, const QDBusMessage& message)
{
    if (message.type() != QDBusMessage::MethodCallMessage) {
        return false;
    }
    const QString called = message.interface();
    if (called.isEmpty()) {
        return true;
    }
    const QMetaObject* const meta = adaptor->metaObject();
    const int info = meta->indexOfClassInfo("D-Bus Interface");
    return info >= 0 && called == QLatin1String(meta->classInfo(info).value());
}

} // namespace

bool sendBusError(const QObject* adaptor, QDBusError::ErrorType type, const QString& message)
{
    const QDBusContext* const context = busContext(adaptor);
    if (!context || !isCallToOwnInterface(adaptor, context->message())) {
        return false;
    }
    context->sendErrorReply(type, message);
    return true;
}

std::optional<QString> busCallerOf(const QObject* adaptor, QLatin1StringView member)
{
    const QDBusContext* const context = busContext(adaptor);
    if (!context) {
        return std::nullopt;
    }
    const QDBusMessage& message = context->message();
    if (message.member() != member || !isCallToOwnInterface(adaptor, message)) {
        return std::nullopt;
    }
    return message.service();
}

bool isBridgeCaller(const QObject* adaptor, const QString& busName)
{
    // The host's direct children are its adaptors, so the lookup is a few
    // pointer comparisons, and reading the bridge where it lives keeps one
    // record of the registered name.
    const QObject* const host = adaptor ? adaptor->parent() : nullptr;
    const CompositorBridgeAdaptor* const bridge =
        host ? host->findChild<CompositorBridgeAdaptor*>(QString(), Qt::FindDirectChildrenOnly) : nullptr;
    return bridge && bridge->isBridgeService(busName);
}

bool admitCompositorReport(const QObject* adaptor, QLatin1StringView member)
{
    const std::optional<QString> caller = busCallerOf(adaptor, member);
    if (!caller || isBridgeCaller(adaptor, *caller)) {
        return true;
    }
    // Once per caller: a peer that keeps calling would otherwise fill the log.
    // Bounded, because a peer that reconnects comes back under a new unique
    // name each time and the names are never reused: past the bound a refusal
    // logs at debug and the set stops growing.
    constexpr qsizetype MaxWarnedCallers = 32;
    static QSet<QString> warned;
    if (warned.contains(*caller)) {
        qCDebug(lcDbus) << member << "refused for" << *caller;
    } else if (warned.size() < MaxWarnedCallers) {
        warned.insert(*caller);
        qCWarning(lcDbus) << member << "refused for" << *caller << "- reserved for the registered compositor bridge";
    } else {
        qCDebug(lcDbus) << member << "refused for" << *caller << "- reserved for the registered compositor bridge";
    }
    sendBusError(adaptor, QDBusError::AccessDenied,
                 QStringLiteral("%1 is reserved for the registered compositor bridge").arg(member));
    return false;
}

} // namespace DbusHelpers
} // namespace PlasmaZones
