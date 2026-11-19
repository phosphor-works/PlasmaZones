// SPDX-FileCopyrightText: 2026 fuddlesworth
// SPDX-License-Identifier: GPL-3.0-or-later

#include "shaderadaptor.h"
#include "common/engineruntimenamespace.h"

#include <PhosphorEngineRuntime/ShaderService.h>

namespace PlasmaZones {

ShaderAdaptor::ShaderAdaptor(ShaderService* service, QObject* parent)
    : QDBusAbstractAdaptor(parent)
    , m_service(service)
{
    Q_ASSERT(service);
    connect(service, &ShaderService::shaderCompilationStarted, this, &ShaderAdaptor::shaderCompilationStarted);
    connect(service, &ShaderService::shaderCompilationFinished, this, &ShaderAdaptor::shaderCompilationFinished);
    connect(service, &ShaderService::shadersChanged, this, &ShaderAdaptor::shadersChanged);
}

ShaderAdaptor::~ShaderAdaptor() = default;

QVariantList ShaderAdaptor::availableShaders()
{
    return m_service ? m_service->availableShaders() : QVariantList{};
}

QVariantMap ShaderAdaptor::shaderInfo(const QString& shaderId)
{
    return m_service ? m_service->shaderInfo(shaderId) : QVariantMap{};
}

QVariantMap ShaderAdaptor::defaultShaderParams(const QString& shaderId)
{
    return m_service ? m_service->defaultShaderParams(shaderId) : QVariantMap{};
}

QVariantMap ShaderAdaptor::translateShaderParams(const QString& shaderId, const QVariantMap& params)
{
    return m_service ? m_service->translateShaderParams(shaderId, params) : QVariantMap{};
}

bool ShaderAdaptor::shadersEnabled()
{
    return m_service ? m_service->shadersEnabled() : bool{};
}

bool ShaderAdaptor::userShadersEnabled()
{
    return m_service ? m_service->userShadersEnabled() : bool{};
}

QString ShaderAdaptor::userShaderDirectory()
{
    return m_service ? m_service->userShaderDirectory() : QString{};
}

void ShaderAdaptor::openUserShaderDirectory()
{
    if (m_service) {
        m_service->openUserShaderDirectory();
    }
}

void ShaderAdaptor::refreshShaders()
{
    if (m_service) {
        m_service->refreshShaders();
    }
}

} // namespace PlasmaZones
