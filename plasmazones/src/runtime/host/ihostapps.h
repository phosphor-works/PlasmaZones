// SPDX-FileCopyrightText: 2026 fuddlesworth
// SPDX-License-Identifier: GPL-3.0-or-later

#pragma once

#include <QString>

namespace PhosphorEngineRuntime {

/**
 * @brief The host's own applications the engine opens on the user's request
 *
 * The settings shortcut and the layout-editor verbs ask for them; which
 * program that is, and how it is started, is the host's. Under PlasmaZones
 * they are plasmazones-settings and plasmazones-editor, started with the
 * environment plasmazonesd inherited rather than the one it set for itself.
 */
class IHostApps
{
public:
    /// What the layout editor should open on. Empty fields ask for nothing in
    /// particular: an all-empty request opens the editor as it is.
    struct EditorRequest
    {
        /// The screen to edit layouts for.
        QString screenId;
        /// A layout to open, on screenId when one is given.
        QString layoutId;
        /// A scrolling template to open, by id.
        QString scrollingTemplateId;
        /// Open a new scrolling template.
        bool newScrollingTemplate = false;
    };

    virtual ~IHostApps() = default;

    /// Open the host's settings application. False when it could not be
    /// started.
    virtual bool openSettings() = 0;

    /// Open the host's layout editor on @p request. False when it could not
    /// be started.
    virtual bool openEditor(const EditorRequest& request) = 0;
};

} // namespace PhosphorEngineRuntime
