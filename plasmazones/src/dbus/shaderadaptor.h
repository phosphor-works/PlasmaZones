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

namespace PlasmaZones {

class ShaderService;

/**
 * @brief The D-Bus binding of ShaderService: org.phosphor_works.Placement.Shader
 *
 * Every method, property and signal here is one the interface XML names, with
 * the signature it pins; each one's behaviour is documented on the
 * ShaderService member of the same name. The binding forwards and relays
 * the service's signals.
 */
class PLASMAZONES_DBUS_EXPORT ShaderAdaptor : public QDBusAbstractAdaptor
{
    Q_OBJECT
    Q_CLASSINFO("D-Bus Interface", "org.phosphor_works.Placement.Shader")

public:
    /// @param service Not owned. A call that arrives after it is gone is answered empty.
    /// @param parent The object the adaptor is exported through
    ShaderAdaptor(ShaderService* service, QObject* parent);
    ~ShaderAdaptor() override;

public Q_SLOTS:
    QVariantList availableShaders();
    QVariantMap shaderInfo(const QString& shaderId);
    QVariantMap defaultShaderParams(const QString& shaderId);
    QVariantMap translateShaderParams(const QString& shaderId, const QVariantMap& params);
    bool shadersEnabled();
    bool userShadersEnabled();
    QString userShaderDirectory();
    void openUserShaderDirectory();
    void refreshShaders();

Q_SIGNALS:
    void shaderCompilationStarted(const QString& shaderId);
    void shaderCompilationFinished(const QString& shaderId, bool success, const QString& error);
    void shadersChanged();

private:
    QPointer<ShaderService> m_service;
};

} // namespace PlasmaZones
