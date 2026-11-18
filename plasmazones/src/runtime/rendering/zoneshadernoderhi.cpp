// SPDX-FileCopyrightText: 2026 fuddlesworth
// SPDX-License-Identifier: GPL-3.0-or-later

#include "zoneshadernoderhi.h"

#include <PhosphorRendering/ShaderCompiler.h>

namespace PhosphorEngineRuntime {

PhosphorRendering::WarmShaderBakeResult
warmShaderBakeCacheForPaths(const QString& vertexPath, const QString& fragmentPath, const QStringList& includePaths,
                            const QString& paramPreamble, const QString& entryPrologue,
                            const QList<PhosphorShaders::EntryCandidate>& entryCandidates)
{
    if (vertexPath.isEmpty() || fragmentPath.isEmpty()) {
        PhosphorRendering::WarmShaderBakeResult result;
        result.errorMessage = QStringLiteral("Vertex or fragment path is empty");
        return result;
    }

    // The bake resolves #include against the bundled shared/ directories too,
    // as the live render node does.
    return PhosphorRendering::warmShaderBakeCacheForPaths(vertexPath, fragmentPath,
                                                          expandShaderIncludePaths(includePaths), paramPreamble,
                                                          entryPrologue, entryCandidates);
}

} // namespace PhosphorEngineRuntime
