// SPDX-FileCopyrightText: 2026 Phosphor Works Team
// SPDX-License-Identifier: GPL-3.0-or-later

#pragma once

#include "plasmazones_dbus_export.h"

#include <QDBusError>
#include <QLatin1StringView>
#include <QString>

#include <optional>

class QDBusContext;
class QObject;

namespace PlasmaZones {

/**
 * @brief The bus-context helpers the D-Bus adaptors share: who is calling,
 *        whether the caller is the registered compositor bridge, and how to
 *        answer the call being served with an error.
 *
 * The lookup and validation helpers the services themselves use are
 * Lookups (runtime/common/lookups.h).
 */
namespace DbusHelpers {

// ═══════════════════════════════════════════════════════════════════════════════
// Bus context (implemented in dbushelpers.cpp)
// ═══════════════════════════════════════════════════════════════════════════════

/**
 * @brief The context of the bus message the object @p adaptor is exported
 *        through is serving now, or null while it serves none (a direct
 *        call, as the unit tests make).
 *
 * That message is whatever the host is delivering: a call to this adaptor or
 * to another one on the same host, or a bus signal delivered to a host slot,
 * so code reached from any of those reads it, not only the code of this
 * adaptor's own slot. The context's connection() is the connection that
 * message came in on.
 *
 * Qt sets a call's QDBusContext on the object the adaptor is exported
 * through, its parent, never on the QDBusAbstractAdaptor itself (see the
 * QDBusContext documentation), so an adaptor that inherits QDBusContext reads
 * calledFromDBus() as false on every call and its sendErrorReply never
 * reaches anyone. The parent has to inherit it (Daemon does); a parent that
 * does not answers null.
 */
PLASMAZONES_DBUS_EXPORT const QDBusContext* busContext(const QObject* adaptor);

/**
 * @brief The unique bus name of the sender of the message busContext()
 *        reads, or empty while the host serves none.
 */
PLASMAZONES_DBUS_EXPORT QString busCaller(const QObject* adaptor);

/**
 * @brief The unique bus name of the sender of the call @p adaptor is serving,
 *        when that call is @p member on @p adaptor's own D-Bus interface (or
 *        names none, which Qt dispatches by member name); nullopt otherwise.
 *
 * Nullopt for a direct call, and for a slot reached in-process while the host
 * serves another message: that message's sender is not this slot's caller.
 * A served call can still answer an empty name (a peer-to-peer connection
 * names no sender), which matches no registered bridge.
 */
PLASMAZONES_DBUS_EXPORT std::optional<QString> busCallerOf(const QObject* adaptor, QLatin1StringView member);

/**
 * @brief Whether @p busName is the unique bus name the compositor bridge
 *        registered from, read from the CompositorBridgeAdaptor on
 *        @p adaptor's host.
 *
 * The one test of "the caller is the registered bridge": admitCompositorReport
 * applies it to compositor-only slots, and the daemon hands it to the adaptors
 * that treat the bridge's calls differently from other callers'.
 */
PLASMAZONES_DBUS_EXPORT bool isBridgeCaller(const QObject* adaptor, const QString& busName);

/**
 * @brief Answer the bus call @p adaptor is serving with the error @p type,
 *        and report whether a reply went out.
 *
 * Only a method call addressed to @p adaptor's own D-Bus interface is
 * answered, or one naming no interface, which Qt dispatches by member name.
 * While a direct C++ call reaches this adaptor, the host may be serving a
 * call to another of its adaptors or delivering a bus signal, and neither is
 * this adaptor's to answer.
 *
 * On true Qt marks the reply delayed and discards whatever the slot returns,
 * so the slot's return value matters only on false, where it is the answer:
 * to a direct call, or to a bus call this adaptor does not own.
 */
PLASMAZONES_DBUS_EXPORT bool sendBusError(const QObject* adaptor, QDBusError::ErrorType type, const QString& message);

/**
 * @brief The first statement of a compositor-only slot (#1155): true when the
 *        slot may proceed.
 *
 * A compositor-only method reports what the compositor saw, and the engines
 * take it as ground truth, so only the registered compositor bridge may call
 * it over the bus: the unique bus name CompositorBridgeAdaptor recorded at
 * registerBridge, read from the bridge adaptor on @p adaptor's host so there
 * is one record of it. A bus caller with any other name, or any caller before
 * a bridge registered over the bus, is answered with AccessDenied and the slot
 * returns at once.
 *
 * Admitted without a check: a direct C++ call (no bus context, as the unit
 * tests and the daemon's own calls make), and a call reached while the host
 * serves a different message, which is the case when another slot calls this
 * one (endDrag calling windowScreenChanged, say). That outer slot answers for
 * its own caller. So the check runs only when the message being served is a
 * method call of @p member on @p adaptor's own interface, or one naming no
 * interface, which Qt dispatches by member name (busCallerOf).
 *
 * The identity is a bus connection, not a component: everything that shares
 * the compositor's connection passes too. Under KWin that is every effect and
 * every KWin script, since they all call through KWin's one session-bus
 * connection. Registration is held to the compositor's process
 * (CompositorBridgeAdaptor::setCompositorIdentityCheck), so a peer outside it
 * never passes.
 *
 * A refused caller is logged once at warning, for a bounded number of
 * distinct callers, and at debug after that.
 */
PLASMAZONES_DBUS_EXPORT bool admitCompositorReport(const QObject* adaptor, QLatin1StringView member);

} // namespace DbusHelpers
} // namespace PlasmaZones
