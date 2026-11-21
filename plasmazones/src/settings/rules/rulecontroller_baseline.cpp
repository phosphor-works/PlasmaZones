// SPDX-FileCopyrightText: 2026 fuddlesworth
// SPDX-License-Identifier: GPL-3.0-or-later

// User-rule dirty check, profile staging and the Restore Defaults rule reload
// for RuleController. The controller's class definition spans this TU and
// rulecontroller.cpp.
//
// Covers: the value-based user-rule dirty check (userRulesDirty against the last
// daemon-synced snapshot), staging a profile's resolved rules (stageUserRules)
// and the fire-and-forget daemon rule reload (reloadDaemonRules).

#include "rulecontroller.h"
#include <PhosphorEngineRuntime/ConfigLogging.h>
#include "common/engineruntimenamespace.h"

#include <PhosphorEngineRuntime/Logging.h>

#include <PhosphorProtocol/ClientHelpers.h>
#include <PhosphorProtocol/ServiceConstants.h>
#include <PhosphorRules/Rule.h>

#include <QList>
#include <QString>

namespace PlasmaZones {

void RuleController::captureSavedSnapshot()
{
    m_savedRules = m_model.rules();
}

bool RuleController::userRulesDirty() const
{
    return m_model.rules() != m_savedRules;
}

void RuleController::stageUserRules(const QList<PhosphorRules::Rule>& userRules)
{
    // Replace the model with the profile's resolved rules. The incoming list
    // already carries the desired order; renormalizePriorities re-stamps
    // priority from list order, exactly as the CRUD path.
    QList<PhosphorRules::Rule> next;
    next.reserve(userRules.size());
    for (const PhosphorRules::Rule& r : userRules) {
        // Boundary defense for this PUBLIC entry point, mirroring addRule:
        // one invalid rule in the model poisons the eventual Save whole
        // (RuleSet::setRules drops it, accepted < size, and the entire push
        // is rejected with a generic error the user cannot trace). Today's
        // sole producer (ProfileStore::resolveRules) cannot deliver one —
        // Rule::fromJson validates every clause isValid() checks — so this
        // guards direct and future callers, not a loader gap.
        if (!r.isValid()) {
            qCWarning(lcConfig) << "stageUserRules: dropping an invalid profile rule" << r.id;
            continue;
        }
        next.append(r);
    }
    m_model.setRules(next);
    renormalizePriorities();

    // Deliberately NOT captureSavedSnapshot(): the staged rules must read dirty
    // so the Rules page badges AND the global Save commits them. Badging and
    // committing ride two different mechanisms and BOTH need driving here:
    // reconcileRuleBackedDirty derives the badge from the value-based
    // userRulesDirty(), but asyncCommit gates the actual setAllRules push on
    // m_dirty, and reload() overwrites the model from the daemon whenever
    // m_dirty is false — so a bare dirtyChanged emit badged the page while the
    // Save silently pushed nothing and the next rulesChanged broadcast erased
    // the staged rules. setDirty(true) flips the commit gate and emits on the
    // transition; the re-stage-while-already-dirty case still needs its own
    // emit so the reconcile re-runs against the new model contents.
    const bool wasDirty = m_dirty;
    setDirty(true);
    if (wasDirty) {
        Q_EMIT dirtyChanged();
    }
}

void RuleController::reloadDaemonRules()
{
    // Fire-and-forget: no out-args, and the model refresh comes from the daemon's
    // rulesChanged broadcast + the revert() the caller issues. Ordered before any
    // subsequent getAllRules on this connection, so a paired revert() sees the
    // reloaded set.
    PhosphorProtocol::ClientHelpers::asyncCall(QString(PhosphorProtocol::Service::Interface::Rules),
                                               QStringLiteral("reloadRules"));
}

} // namespace PlasmaZones
