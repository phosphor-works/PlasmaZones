// SPDX-FileCopyrightText: 2026 fuddlesworth
// SPDX-License-Identifier: GPL-3.0-or-later

#include "plasmazonesapps.h"

#include "daemon/host/childprocessenvironment.h"
#include "core/platform/logging.h"

#include <QCoreApplication>
#include <QFile>
#include <QProcess>
#include <QStandardPaths>
#include <QStringList>

namespace PlasmaZones {

bool PlasmaZonesApps::openSettings()
{
    const QProcessEnvironment env = childProcessEnvironment();
    // systemd-run --scope runs the command as a child of this call, so the
    // environment set here is what the settings app inherits.
    QProcess scoped;
    scoped.setProgram(QStringLiteral("systemd-run"));
    scoped.setArguments({QStringLiteral("--user"), QStringLiteral("--scope"), QStringLiteral("plasmazones-settings")});
    scoped.setProcessEnvironment(env);
    if (scoped.startDetached()) {
        return true;
    }
    QProcess direct;
    direct.setProgram(QStringLiteral("plasmazones-settings"));
    direct.setProcessEnvironment(env);
    return direct.startDetached();
}

bool PlasmaZonesApps::openEditor(const EditorRequest& request)
{
    // Resolved once for the daemon's lifetime, deliberately: the fallback is
    // the bare name, which QProcess re-resolves against PATH at every spawn,
    // so an editor installed after daemon start still launches. Only a
    // package move to a different prefix goes stale until a daemon restart.
    static const QString editor = []() {
        const QString found = QStandardPaths::findExecutable(QStringLiteral("plasmazones-editor"));
        if (!found.isEmpty()) {
            return found;
        }
        const QString local = QCoreApplication::applicationDirPath() + QStringLiteral("/plasmazones-editor");
        if (QFile::exists(local)) {
            return local;
        }
        return QStringLiteral("plasmazones-editor");
    }();

    // The editor's command line. A screen id goes through verbatim: the
    // editor matches it against QScreen::name() and looks the geometry up.
    QStringList args;
    if (request.newScrollingTemplate) {
        args << QStringLiteral("--new-scrolling-template");
    } else if (!request.scrollingTemplateId.isEmpty()) {
        args << QStringLiteral("--scrolling-template") << request.scrollingTemplateId;
    } else {
        if (!request.layoutId.isEmpty()) {
            args << QStringLiteral("--layout") << request.layoutId;
        }
        if (!request.screenId.isEmpty()) {
            args << QStringLiteral("--screen") << request.screenId;
        }
    }

    QProcess process;
    process.setProgram(editor);
    process.setArguments(args);
    process.setProcessEnvironment(childProcessEnvironment());
    return process.startDetached();
}

} // namespace PlasmaZones
