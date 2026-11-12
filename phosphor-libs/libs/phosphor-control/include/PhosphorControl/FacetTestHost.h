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
/// run() copies the module library, and every target module
/// (addTargetModule()), into a private search path, hosts them in a FacetHost
/// over a fresh ApplicationController, loads the module, and checks that:
///   - the catalog accepts it (IID, ApiVersion, a valid "Id")
///   - its metadata is valid, and parses without a warning: a key this Facet
///     does not know, a search entry of a kind it does not know, or a
///     `Key[<lang>]` no locale picks is reported as `metadata: <warning>`,
///     since the module is built against the Facet that checks it
///   - it loads, and registerInto registers exactly the pages and extensions
///     its metadata declares, each page with a controller of the matching id
///     and an absolute QML source (the rules FacetContext enforces)
///   - every page was bound, and every headless domain and every extension's
///     domain is still there, which code the module runs while it loads
///     could delete, at once or with a deleteLater()
///   - each page's and each extension's QML source exists, for a `qrc:` or
///     `file:` URL
///   - no page controller, headless domain or extension domain is dirty
///     right after loading
///   - every page controller, headless domain and extension domain answers
///     apply() with one applyResult and discard() with one discardResult,
///     both reporting success, sends no second one within a short grace
///     period, and is not deleted by either call or by another domain's
///   - a module whose metadata says `SearchProviders` registered one
///   - every entry its search providers return, except an action (which has
///     no page), points at one of its pages or a page it extends. A setting
///     or section entry with an anchor names one its page lists, on its own
///     page, and on a page it extends when that page's module is a target.
///     One with no anchor points at the page as a whole and passes.
///   - with addTargetModule(), every extension aimed at a target module fits
///     the slot and version that module offers
///   - with setPreviousAnchors(), no page of the previous release is gone, no
///     anchor is gone unless the page's anchors version went up, the anchors
///     version did not go down, and no named extension point is gone or went
///     down in version. A page can never go, since other modules may extend
///     it, whatever its version.
///
/// When the module is refused, its metadata is invalid or it does not load,
/// run() reports that and stops, since the later checks need a loaded
/// module. The checks after loading all run and report together. A problem
/// with a target module is reported too, by the path it was added with, and
/// the run goes on without it: a target the catalog refuses, and one whose id
/// is the module's or another target's, which is left out.
///
/// The apply and discard round runs on freshly loaded, clean state, which is
/// not how Facet drives a domain: Facet applies dirty domains only. It
/// catches a domain that answers twice or not at all, not one whose dirty
/// path misbehaves, so cover that in the module's own tests.
///
/// run() does not write the settings files the module opens through
/// FacetContext::configBackend, as long as the module's factory opens the
/// path it is handed. It makes each of them under its staging
/// directory instead (FacetHost::setConfigRedirect), so the module starts
/// from an empty file and its defaults, and the files go when run() returns.
/// Anything else a domain's apply() writes (a file the module opens by
/// itself, a call to a daemon) still goes where the module sends it, so
/// point that at a scratch target first.
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
    /// Where run() stages its copies: a fresh directory
    /// `<directory>/facet-test-XXXXXX/`, removed when run() returns, whose
    /// `facet/` holds the module as `module.so` and the target modules as
    /// `target-000.so` onwards, whatever their files are called. A relative
    /// @p directory is taken against the working directory of run().
    /// Default: QDir::tempPath(). Pick a directory on a filesystem that allows
    /// executing libraries when /tmp is mounted noexec. A module that finds
    /// its own libraries through an $ORIGIN rpath needs them reachable from
    /// that copy, or an absolute rpath.
    void setStagingDirectory(const QString& directory);

    /// Offer the module at @p modulePath too, so the module under test's
    /// extensions aimed at it, and its search entries on that module's pages,
    /// are checked against what its metadata declares. A target is never
    /// loaded, and its own metadata and extensions are not checked.
    void addTargetModule(const QString& modulePath);

    /// The anchors.json of the module's previous release
    /// (phosphor_add_facet_module_metadata's FACET_ANCHORS_FILE), which
    /// holds each page's anchors, its anchors version and the versions of its
    /// named extension points.
    void setPreviousAnchors(const QString& anchorsPath);

    /// Every problem found, one line each. Empty means the module passes.
    [[nodiscard]] QStringList run();

private:
    QString m_modulePath;
    QStringList m_targetPaths;
    QString m_previousAnchorsPath;
    QString m_stagingDirectory;
    int m_resultTimeoutMs = DefaultResultTimeoutMs;
};

} // namespace PhosphorControl
