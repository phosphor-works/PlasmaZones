// SPDX-FileCopyrightText: 2026 Phosphor Works Team
// SPDX-License-Identifier: GPL-3.0-or-later

#include "compositorbridgeadaptor.h"
#include "common/engineruntimenamespace.h"
#include <PhosphorEngineRuntime/Constants.h>
#include <PhosphorEngineRuntime/Logging.h>
#include "dbus/dbushelpers.h"

#include <PhosphorProtocol/ServiceConstants.h>
#include <PhosphorProtocol/BridgeMarshalling.h>
#include <QDBusConnection>
#include <QDBusContext>
#include <QDBusMessage>
#include <QUuid>

#include <optional>

namespace PlasmaZones {

// Protocol version constants are the single source of truth in
// PhosphorProtocol::Service (libs/phosphor-protocol). The effect and the
// daemon both read them from PhosphorProtocol/ServiceConstants.h.
namespace {
constexpr int DaemonApiVersion = PhosphorProtocol::Service::ApiVersion;
constexpr int DaemonMinPeerApiVersion = PhosphorProtocol::Service::MinPeerApiVersion;
} // namespace

CompositorBridgeAdaptor::CompositorBridgeAdaptor(QObject* parent)
    : QDBusAbstractAdaptor(parent)
{
}

PhosphorProtocol::BridgeRegistrationResult CompositorBridgeAdaptor::registerBridge(const QString& compositorName,
                                                                                   const QString& version,
                                                                                   const QStringList& capabilities)
{
    const auto rejected = [&compositorName] {
        PhosphorProtocol::BridgeRegistrationResult result;
        result.apiVersion = QString::number(DaemonApiVersion);
        result.bridgeName = compositorName;
        result.sessionId = QStringLiteral("REJECTED");
        return result;
    };

    // Input validation at the D-Bus boundary: an empty compositorName would
    // commit a registration that isBridgeRegistered() (keyed on a non-empty
    // name) can never observe, silently skipping the re-registration warning
    // on the next register too. Reject before any state changes.
    if (compositorName.isEmpty()) {
        qCWarning(lcDbusWindow) << "Compositor bridge REJECTED: empty compositorName";
        return rejected();
    }

    // Version gate: reject effects that speak an older protocol version.
    // The effect passes its apiVersion as the `version` string.
    const int peerApiVersion = version.toInt();
    if (peerApiVersion < DaemonMinPeerApiVersion) {
        qCWarning(lcDbusWindow) << "Compositor bridge REJECTED: peer apiVersion" << peerApiVersion << "is below minimum"
                                << DaemonMinPeerApiVersion << "(compositor:" << compositorName << ")."
                                << "Update the effect to match the daemon.";
        return rejected();
    }

    // The registered bus name is what every compositor-only method admits
    // (DbusHelpers::admitCompositorReport), so registering hands the caller
    // the compositor's methods (#1155). Read only while serving a
    // registerBridge call on this interface: reached in-process from another
    // slot, the message being served is that slot's caller, not a registrant.
    const std::optional<QString> served = DbusHelpers::busCallerOf(this, QLatin1StringView("registerBridge"));
    const QString caller = served.value_or(QString());

    // Only the compositor's own process registers. First come was not enough:
    // a peer that registered while no bridge was on the bus (before KWin's
    // effect loaded, or between a daemon restart and the effect's
    // re-registration) held the registration for as long as it stayed
    // connected, and the gate below then refused the compositor itself. The
    // names logged here are bus names, never the caller's compositorName.
    // The check fails closed: a GetConnectionUnixProcessID lookup that errors
    // or times out (OverlayAdaptor::isCompositorPeer) answers false for the
    // real compositor too, and that refusal costs what the occupied refusal
    // below describes, an effect left idle until the next daemonReady or its
    // own reload, because it does not retry a REJECTED reply.
    if (served && !(m_compositorIdentityCheck && m_compositorIdentityCheck(caller))) {
        if (!m_identityRefusalLogged) {
            m_identityRefusalLogged = true;
            qCWarning(lcDbusWindow) << "Compositor bridge REJECTED:" << caller
                                    << "is not the compositor (further refusals log at debug)";
        } else {
            qCDebug(lcDbusWindow) << "Compositor bridge REJECTED:" << caller << "is not the compositor";
        }
        return rejected();
    }

    // One bridge at a time. The same name registering again (the effect
    // reloading in a running KWin) and a new name once the old one has left
    // the bus (KWin or the daemon restarted) are let through. The old name's
    // owner is asked with a bounded call, and an answer that does not say it
    // is gone keeps the registration, a failed or timed-out query included.
    // That refusal is not cheap: the effect does not retry a REJECTED reply
    // (nor an error one), so it stays idle until the daemon next announces
    // daemonReady, which in practice means a daemon restart, or the effect
    // itself is reloaded. It is still the
    // side to fail on, because a wrong admission would hand the compositor's
    // methods to a second connection for as long as it stays on the bus.
    if (served && !m_bridgeService.isEmpty() && caller != m_bridgeService
        && !bridgeServiceGone(DbusHelpers::busContext(this))) {
        if (!m_occupiedRefusalLogged) {
            m_occupiedRefusalLogged = true;
            qCWarning(lcDbusWindow) << "Compositor bridge REJECTED:" << caller << "tried to register while"
                                    << m_bridgeService << "is still registered (further refusals log at debug)";
        } else {
            qCDebug(lcDbusWindow) << "Compositor bridge REJECTED:" << caller << "while" << m_bridgeService
                                  << "is still registered";
        }
        return rejected();
    }

    // Logged only once every rejection gate has passed: a rejected peer
    // replaces nothing, so warning before the gates would mislead.
    if (!m_bridgeName.isEmpty()) {
        qCWarning(lcDbusWindow) << "Compositor bridge re-registration: replacing" << m_bridgeName << m_bridgeVersion
                                << "with" << compositorName << version;
    }

    m_bridgeName = compositorName;
    m_bridgeVersion = version;
    m_capabilities = capabilities;
    // Remember WHO registered: the identity every compositor-only method
    // admits from now on (DbusHelpers::admitCompositorReport). A direct
    // (non-D-Bus) call has no peer and leaves this empty, which admits no
    // bus caller.
    m_bridgeService = caller;
    // A refusal after this registration is news again.
    m_identityRefusalLogged = false;
    m_occupiedRefusalLogged = false;

    qCInfo(lcDbusWindow) << "Compositor bridge registered:" << compositorName << "apiVersion=" << version
                         << "capabilities:" << capabilities;

    Q_EMIT bridgeRegistered(compositorName, version, capabilities);

    PhosphorProtocol::BridgeRegistrationResult result;
    result.apiVersion = QString::number(DaemonApiVersion);
    result.bridgeName = compositorName;
    result.sessionId = QUuid::createUuid().toString(QUuid::WithoutBraces);
    return result;
}

bool CompositorBridgeAdaptor::bridgeServiceGone(const QDBusContext* context) const
{
    if (!context) {
        return false;
    }
    QDBusMessage query =
        QDBusMessage::createMethodCall(QStringLiteral("org.freedesktop.DBus"), QStringLiteral("/org/freedesktop/DBus"),
                                       QStringLiteral("org.freedesktop.DBus"), QStringLiteral("NameHasOwner"));
    query << m_bridgeService;
    // Bounded: the dbus-daemon answers from a hash lookup, and Qt's default
    // timeout would hold the daemon's main thread for 25 s.
    const QDBusMessage answer =
        context->connection().call(query, QDBus::Block, PhosphorProtocol::Service::SyncCallTimeoutMs);
    return answer.type() == QDBusMessage::ReplyMessage && !answer.arguments().isEmpty()
        && !answer.arguments().constFirst().toBool();
}

void CompositorBridgeAdaptor::reportModifierState(int modifiers, int mouseButtons)
{
    if (!DbusHelpers::admitCompositorReport(this, QLatin1StringView(__func__))) {
        return;
    }
    Q_EMIT modifierStateChanged(modifiers, mouseButtons);
}

} // namespace PlasmaZones
