// SPDX-FileCopyrightText: 2026 fuddlesworth
// SPDX-License-Identifier: LGPL-2.1-or-later
#pragma once

#include "phosphorcontroltest_export.h"

#include <QString>
#include <QStringList>

namespace PhosphorControl {

/// Checks one Facet module without a window, for the module's own CI. Lives
/// in the PhosphorControl::PhosphorControlTest library.
///
/// run() copies the module library into a private search path, hosts it in a
/// FacetHost over a fresh ApplicationController, loads it, and checks that:
///   - the catalog accepts it (IID, ApiVersion, a valid "Id")
///   - its metadata is valid
///   - it loads, and registerInto registers exactly the pages and extensions
///     its metadata declares, each page with a controller of the matching id
///     and an absolute QML source (the rules FacetContext enforces)
///   - each page's QML source exists, for a `qrc:` or `file:` URL
///   - no page controller, headless domain or extension domain is dirty
///     right after loading
///   - every page controller, headless domain and extension domain answers
///     apply() with one applyResult and discard() with one discardResult,
///     both reporting success, and sends no second one within a short grace
///     period
///
/// When the module is refused, its metadata is invalid or it does not load,
/// run() reports that and stops, since the later checks need a loaded
/// module. The checks after loading all run and report together.
///
/// The apply and discard round runs on freshly loaded, clean state, which is
/// not how Facet drives a domain: Facet applies dirty domains only. It
/// catches a domain that answers twice or not at all, not one whose dirty
/// path misbehaves, so cover that in the module's own tests. A domain whose
/// apply() writes regardless of what is staged writes wherever it is
/// configured to, so point the module at a scratch backend first.
///
/// Needs a QCoreApplication (or QGuiApplication) running on this thread.
class PHOSPHORCONTROLTEST_EXPORT FacetTestHost
{
public:
    static constexpr int DefaultResultTimeoutMs = 2000;

    explicit FacetTestHost(QString modulePath);

    /// How long to wait for a domain's applyResult or discardResult. Zero or
    /// negative restores DefaultResultTimeoutMs.
    void setResultTimeoutMs(int ms);
    /// Where run() stages its copy of the module: the copy lands in
    /// `<directory>/facet-test-XXXXXX/facet/`, under a fresh random name.
    /// Default: QDir::tempPath(). Pick a directory on a filesystem that allows
    /// executing libraries when /tmp is mounted noexec. A module that finds
    /// its own libraries through an $ORIGIN rpath needs them reachable from
    /// that copy, or an absolute rpath.
    void setStagingDirectory(const QString& directory);

    /// Every problem found, one line each. Empty means the module passes.
    [[nodiscard]] QStringList run();

private:
    QString m_modulePath;
    QString m_stagingDirectory;
    int m_resultTimeoutMs = DefaultResultTimeoutMs;
};

} // namespace PhosphorControl
