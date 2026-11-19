// SPDX-FileCopyrightText: 2026 fuddlesworth
// SPDX-License-Identifier: GPL-3.0-or-later

#include "shaderservice.h"
#include <PhosphorEngineRuntime/ShaderRegistry.h>

namespace PhosphorEngineRuntime {

ShaderService::ShaderService(ShaderRegistry* registry, QObject* parent)
    : QObject(parent)
    , m_registry(registry)
{
    // Relay ShaderRegistry signals to D-Bus
    if (m_registry) {
        connect(m_registry, &ShaderRegistry::shaderCompilationStarted, this, &ShaderService::shaderCompilationStarted);
        connect(m_registry, &ShaderRegistry::shaderCompilationFinished, this,
                &ShaderService::shaderCompilationFinished);
        connect(m_registry, &ShaderRegistry::shadersChanged, this, &ShaderService::shadersChanged);
    }
}

QVariantList ShaderService::availableShaders()
{
    return m_registry ? m_registry->availableShadersVariant() : QVariantList();
}

QVariantMap ShaderService::shaderInfo(const QString& shaderId)
{
    return m_registry ? m_registry->shaderInfo(shaderId) : QVariantMap();
}

QVariantMap ShaderService::defaultShaderParams(const QString& shaderId)
{
    return m_registry ? m_registry->defaultParams(shaderId) : QVariantMap();
}

QVariantMap ShaderService::translateShaderParams(const QString& shaderId, const QVariantMap& params)
{
    return m_registry ? m_registry->translateParamsToUniforms(shaderId, params) : QVariantMap();
}

bool ShaderService::shadersEnabled()
{
    return m_registry ? m_registry->shadersEnabled() : false;
}

bool ShaderService::userShadersEnabled()
{
    return m_registry ? m_registry->userShadersEnabled() : false;
}

QString ShaderService::userShaderDirectory()
{
    return m_registry ? m_registry->userShaderDirectory() : QString();
}

void ShaderService::openUserShaderDirectory()
{
    if (m_registry) {
        m_registry->openUserShaderDirectory();
    }
}

void ShaderService::refreshShaders()
{
    if (m_registry) {
        m_registry->refresh();
    }
}

void ShaderService::detach()
{
    if (!m_registry) {
        return;
    }
    disconnect(m_registry, nullptr, this, nullptr);
    m_registry = nullptr;
}

} // namespace PhosphorEngineRuntime
