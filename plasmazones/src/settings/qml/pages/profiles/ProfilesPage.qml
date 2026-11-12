// SPDX-FileCopyrightText: 2026 fuddlesworth
// SPDX-License-Identifier: GPL-3.0-or-later

import QtQuick
import QtQuick.Controls
import QtQuick.Dialogs
import QtQuick.Layouts
import org.kde.kirigami as Kirigami
import org.phosphor.control
import "../../js/ExportFileName.js" as ExportFileName

/**
 * @brief Settings profiles: save the current configuration as a named profile,
 *        switch between profiles, and import/export them.
 *
 * A profile stores only what differs from its parent (another profile, or the
 * defaults at the root). Activating a profile STAGES its settings into the Save
 * footer — the change takes effect when the user saves, and reverts on discard.
 *
 * Talks to `controller.bridge` (a PlasmaZones::ProfileStore, owned by the
 * ProfilePageController that PageHost passes in as `controller`):
 *   QVariantList availableProfiles()   // rows: id, name, description,
 *                                      //   parentId, parentName, isRoot, depth,
 *                                      //   active, modified, signature
 *   QString createProfile(name, description, parentId)
 *   bool    renameProfile(id, newName, description)
 *   bool    updateProfileFromCurrent(id)
 *   QString duplicateProfile(id)
 *   bool    setParent(id, parentId)
 *   QStringList selfAndDescendantIds(id)
 *   bool    removeProfile(id)
 *   bool    activateProfile(id)
 *   bool    exportProfile(id, localPath)
 *   QString importProfile(pathOrUrl)
 *   bool    revertConfigChange(id, changeRow)
 *   bool    revertRuleChange(id, ruleId)
 *   void    openProfilesDirectory()
 *   signal  profilesChanged()
 *   signal  toastRequested(text)
 */
SettingsFlickable {
    id: root

    // PageHost sets both before the page's bindings run, because this page's
    // registry entry passes context (PageRegistry::Entry::passesContext).
    // settingsApp is declared even though this page does not read it: PageHost
    // passes it to every such page, and assigning it to a page that lacks the
    // property logs an error on every build.
    required property QtObject controller
    required property ApplicationController settingsApp

    // The controller's CONSTANT store, so never null once the required
    // controller is set.
    readonly property QtObject bridge: root.controller.bridge

    // Q_INVOKABLE results are not reactive across the QML boundary — refresh
    // manually on profilesChanged.
    property var profilesList: bridge.availableProfiles()

    contentHeight: content.implicitHeight
    clip: true

    // "Defaults" (empty id) first, then every existing profile — the parent
    // candidates for a new profile.
    readonly property var parentOptions: {
        const rows = [
            {
                "id": "",
                "name": i18n("Defaults")
            }
        ];
        for (let i = 0; i < profilesList.length; ++i)
            rows.push(profilesList[i]);
        return rows;
    }

    /// Ids of the profiles whose rows are expanded. Held HERE because _reload()
    /// replaces the row model wholesale, destroying every delegate along with
    /// its expansion state; rows seed from this map on creation and report
    /// toggles back into it. Mutated in place — the object's identity is the
    /// point, not change notification.
    readonly property var _expandedIds: ({})

    /// Set while the page is hidden and something has moved under the rows,
    /// so the re-read waits until the page is shown again. PageHost keeps a
    /// visited page alive but hidden, and without this a hidden Profiles page
    /// re-read every row on every settings edit anywhere in the app.
    property bool _reloadPending: false

    function _reload() {
        root._reloadPending = false;
        root.profilesList = root.bridge.availableProfiles();
    }

    function _reloadWhenShown() {
        if (root.visible)
            root._reload();
        else
            root._reloadPending = true;
    }

    onVisibleChanged: {
        if (root.visible && root._reloadPending)
            root._reload();
    }

    /// Commit the inline save form, then clear it for the next one (mirrors the
    /// sets pages' save flow).
    function _saveCurrent() {
        const parentId = parentCombo.currentValue !== undefined ? parentCombo.currentValue : "";
        if (root.bridge.createProfile(nameField.text.trim(), descField.text.trim(), parentId).length > 0) {
            nameField.text = "";
            descField.text = "";
            parentCombo.chosenParentId = "";
            parentCombo.currentIndex = 0;
        }
    }

    // Toasts are NOT forwarded here: the bridge's toastRequested is wired at
    // window scope in Main.qml, because the sidebar switcher can raise one
    // while any page is loaded.
    Connections {
        function onProfilesChanged() {
            root._reloadWhenShown();
        }

        target: root.bridge
    }

    // A settings edit doesn't fire profilesChanged, but it can flip the active
    // profile's `modified` state — re-read the rows so the badge stays honest.
    // Debounced: settingsChanged fires per property change (a slider drag emits
    // a burst), and each reload exports the whole live config to compare it
    // with the active profile, then rebuilds every row delegate. The profile
    // files themselves are cached between mutations, so no disk walk is
    // involved.
    Timer {
        id: settingsEditRefresh

        interval: 250
        onTriggered: root._reloadWhenShown()
    }

    function _settingsEdited() {
        if (root.visible)
            settingsEditRefresh.restart();
        else
            root._reloadPending = true;
    }

    Connections {
        function onSettingsChanged() {
            root._settingsEdited();
        }

        // A reload that moves only Editor.* settings announces them here
        // instead of on settingsChanged, and profiles capture those keys too.
        function onEditorSettingsChanged() {
            root._settingsEdited();
        }

        target: appSettings
    }

    ColumnLayout {
        id: content

        width: parent.width
        spacing: Kirigami.Units.largeSpacing

        Kirigami.InlineMessage {
            Layout.fillWidth: true
            type: Kirigami.MessageType.Information
            visible: true // InlineMessage defaults to hidden; shown as a permanent explainer.
            text: i18n("A profile captures your current settings and rules. Only what differs from its parent profile (or the defaults) is stored. Activating a profile stages its settings. Save to apply, or discard to revert. Per-monitor and other hardware-specific settings are not included, so a profile stays portable between machines.")
        }

        // ── Save current settings as a new profile ──
        SettingsCard {
            Layout.fillWidth: true
            headerText: i18n("Save current settings")
            searchAnchor: "saveCurrent"
            collapsible: true
            initiallyCollapsed: root.profilesList.length > 0

            contentItem: ColumnLayout {
                spacing: Kirigami.Units.smallSpacing

                Label {
                    Layout.fillWidth: true
                    Layout.leftMargin: Kirigami.Units.largeSpacing
                    Layout.rightMargin: Kirigami.Units.largeSpacing
                    text: i18n("Save everything as it is now as a new profile. Pick a parent to store only the differences from it.")
                    color: Kirigami.Theme.disabledTextColor
                    wrapMode: Text.WordWrap
                }

                // Stacked, not side-by-side — mirrors the sets pages: a 50/50
                // row makes the description column unreadably narrow at the
                // typical settings width. Save sits bottom-right so focus order
                // matches typeflow (name → description → parent → save).
                ColumnLayout {
                    Layout.fillWidth: true
                    Layout.leftMargin: Kirigami.Units.largeSpacing
                    Layout.rightMargin: Kirigami.Units.largeSpacing
                    spacing: Kirigami.Units.smallSpacing

                    TextField {
                        id: nameField

                        Layout.fillWidth: true
                        placeholderText: i18n("Profile name…")
                        Accessible.name: i18n("Profile name")
                        onAccepted: if (saveButton.enabled)
                            saveButton.clicked()
                    }

                    TextField {
                        id: descField

                        Layout.fillWidth: true
                        placeholderText: i18n("Description (optional)…")
                        Accessible.name: i18n("Profile description")
                        onAccepted: if (saveButton.enabled)
                            saveButton.clicked()
                    }

                    // Profiles-only addition to the sets form: what the new
                    // profile inherits from, which decides what counts as a
                    // difference worth storing.
                    Label {
                        text: i18n("Inherit from")
                        color: Kirigami.Theme.disabledTextColor
                    }

                    ProfileComboBox {
                        id: parentCombo

                        // The user's pick, held by id so it survives model
                        // reloads: parentOptions rebinds on every
                        // profilesChanged / debounced settings edit, and a
                        // model reassignment resets currentIndex — without
                        // this a half-filled form silently dropped its
                        // chosen parent back to "Defaults".
                        property string chosenParentId: ""

                        Layout.fillWidth: true
                        model: root.parentOptions
                        // No `currentIndex: 0` binding: the ComboBox default is
                        // already 0, and _saveCurrent() resets the selection
                        // imperatively after a save — a binding here would just
                        // be severed by that write.
                        Accessible.name: i18n("Parent profile")
                        onActivated: chosenParentId = currentValue !== undefined ? currentValue : ""
                        onModelChanged: {
                            if (chosenParentId === "")
                                return;
                            const idx = indexOfValue(chosenParentId);
                            currentIndex = idx >= 0 ? idx : 0;
                        }
                    }

                    Button {
                        id: saveButton

                        Layout.alignment: Qt.AlignRight
                        text: i18n("Save")
                        icon.name: "document-save"
                        Accessible.name: i18n("Save the current settings as a profile")
                        // A colliding name is not a conflict here the way it is
                        // for sets: profiles are keyed by uuid and the store
                        // suffixes a duplicate display name, so there is nothing
                        // to overwrite and nothing to confirm.
                        enabled: nameField.text.trim().length > 0
                        onClicked: root._saveCurrent()
                    }
                }
            }
        }

        // ── Import a profile file ──
        ImportDropCard {
            id: importCard

            headerText: i18n("Import a profile")
            searchAnchor: "importProfile"
            description: i18n("Add a profile exported from PlasmaZones.")
            idleText: i18nc("@info drop-zone idle label", "Drop a profile file here to import it")
            hoverText: i18nc("@info drop-zone hover label", "Release to import the profile")
            // Returns the stored name (empty on failure). The store suffixes a
            // name that is already taken, so the file's name can differ from
            // what the list now shows.
            importFn: url => root._importProfile(url)
            openImportDialogFn: () => importDialog.open()
            openFolderFn: () => root.bridge.openProfilesDirectory()
            importButtonAccessibleName: i18n("Import a profile from a file")
            openFolderAccessibleName: i18n("Open the profiles folder")
            successTextFn: name => i18nc("@info profile import success", "Imported profile “%1”.", name)
            // No failureTextFn: every refusal in ProfileStore::importProfile
            // toasts its own cause, and a generic banner beside it said the
            // same thing twice, sometimes with the wrong reason.
        }

        // ── Saved profiles ──
        SettingsCard {
            Layout.fillWidth: true
            headerText: i18n("Profiles")
            searchAnchor: "profilesList"
            headerTrailingText: i18np("%n profile", "%n profiles", root.profilesList.length)

            contentItem: ColumnLayout {
                spacing: 0

                Label {
                    visible: root.profilesList.length === 0
                    text: i18n("No profiles saved yet.")
                    color: Kirigami.Theme.disabledTextColor
                    font.italic: true
                    Layout.fillWidth: true
                    Layout.margins: Kirigami.Units.largeSpacing
                }

                Repeater {
                    model: root.profilesList

                    delegate: ProfileRow {
                        Layout.fillWidth: true
                        bridge: root.bridge
                        Layout.leftMargin: Kirigami.Units.smallSpacing
                        Layout.rightMargin: Kirigami.Units.smallSpacing
                        // Seed from the page-held map: _reload() rebuilds the
                        // model wholesale, so a row mid-expansion (a per-row
                        // revert fires profilesChanged) is destroyed and
                        // recreated — without this it snapped shut. The click
                        // toggle severs the binding, which is fine: creation is
                        // the only moment the seed matters.
                        expanded: root._expandedIds[modelData.id] === true
                        onExpandedChanged: root._expandedIds[modelData.id] = expanded

                        onActivateRequested: root._activate(modelData)
                        onUpdateRequested: root._update(modelData)
                        onRenameRequested: root._rename(modelData)
                        onDuplicateRequested: root._duplicate(modelData)
                        onSetParentRequested: root._setParent(modelData)
                        onExportRequested: root._export(modelData)
                        onDeleteRequested: root._delete(modelData)
                    }
                }
            }
        }
    }

    /// Import @p pathOrUrl, in the shape ImportDropCard's importFn returns:
    /// the name the profile was stored under, or "" when the store refused it
    /// (it has already toasted why).
    function _importProfile(pathOrUrl) {
        const id = root.bridge.importProfile(pathOrUrl);
        if (id.length === 0)
            return "";
        // A fresh read, not profilesList: while the page is hidden the
        // profilesChanged the import fired only marks a reload as pending.
        const rows = root.bridge.availableProfiles();
        for (let i = 0; i < rows.length; ++i) {
            if (rows[i].id === id)
                return rows[i].name;
        }
        // Imported, but not listed under its id. Still a success, which the
        // banner then reports under the file's own name.
        return true;
    }

    // ── Row action handlers (wired from each ProfileRow's signals) ──
    function _activate(m) {
        // Activating stages the profile's whole configuration, which replaces
        // anything the user has edited and not yet saved, possibly on a page
        // they are not looking at. Ask first when there is something to lose.
        if (settingsController.needsSave) {
            activateConfirm.profileId = m.id;
            activateConfirm.profileName = m.name;
            activateConfirm.open();
            return;
        }
        root.bridge.activateProfile(m.id);
    }
    function _update(m) {
        root.bridge.updateProfileFromCurrent(m.id);
    }
    function _duplicate(m) {
        root.bridge.duplicateProfile(m.id);
    }
    function _rename(m) {
        profileDialog.mode = "rename";
        profileDialog.targetId = m.id;
        profileDialog.excludeIds = [];
        profileDialog.nameText = m.name;
        profileDialog.descriptionText = m.description;
        profileDialog.open();
    }
    function _setParent(m) {
        profileDialog.mode = "parent";
        profileDialog.targetId = m.id;
        // The profile itself and everything inheriting from it: the store
        // refuses each of them as a parent, so the picker does not offer them.
        profileDialog.excludeIds = root.bridge.selfAndDescendantIds(m.id);
        profileDialog.parentId = m.parentId;
        profileDialog.open();
    }
    function _export(m) {
        exportDialog.profileId = m.id;
        // A full URL in the dialog's folder, the way the library pages seed
        // their exports. The name keeps every character a file system accepts.
        exportDialog.selectedFile = exportDialog.currentFolder + "/" + ExportFileName.forDisplayName(m.name, "json", i18nc("@item file name for an exported profile whose name has no usable characters", "profile"));
        exportDialog.open();
    }
    function _delete(m) {
        deleteConfirm.profileId = m.id;
        deleteConfirm.profileName = m.name;
        deleteConfirm.open();
    }

    // ── Rename / set-parent form. Creating is inline on the card above. ──
    NewProfileDialog {
        id: profileDialog

        property string targetId: ""

        profiles: root.profilesList
        onProfileAccepted: (name, description, parentId) => {
            if (profileDialog.mode === "rename")
                root.bridge.renameProfile(profileDialog.targetId, name, description);
            else if (profileDialog.mode === "parent")
                root.bridge.setParent(profileDialog.targetId, parentId);
        }
    }

    // ── Activate confirmation, shown only when there are unsaved changes ──
    Kirigami.PromptDialog {
        id: activateConfirm

        property string profileId: ""
        property string profileName: ""

        title: i18n("Use this profile?")
        subtitle: i18n("You have changes you have not saved yet. Switching to “%1” replaces them with that profile's settings.", activateConfirm.profileName)
        standardButtons: Kirigami.Dialog.Cancel
        customFooterActions: [
            Kirigami.Action {
                text: i18n("Use profile")
                icon.name: "dialog-ok-apply"
                onTriggered: {
                    root.bridge.activateProfile(activateConfirm.profileId);
                    activateConfirm.close();
                }
            }
        ]
    }

    // ── Delete confirmation ──
    Kirigami.PromptDialog {
        id: deleteConfirm

        property string profileId: ""
        property string profileName: ""

        title: i18n("Delete profile?")
        subtitle: i18n("“%1” will be permanently deleted. Any profiles inheriting from it will be re-based on its parent, keeping their own settings.", deleteConfirm.profileName)
        standardButtons: Kirigami.Dialog.Cancel
        customFooterActions: [
            Kirigami.Action {
                text: i18n("Delete")
                icon.name: "edit-delete"
                onTriggered: {
                    root.bridge.removeProfile(deleteConfirm.profileId);
                    delete root._expandedIds[deleteConfirm.profileId];
                    deleteConfirm.close();
                }
            }
        ]
    }

    // ── Export / import file pickers ──
    FileDialog {
        id: exportDialog

        property string profileId: ""

        title: i18n("Export Profile")
        nameFilters: [i18n("PlasmaZones Profile (*.json)"), i18n("All files (*)")]
        fileMode: FileDialog.SaveFile
        defaultSuffix: "json"
        // The store toasts the outcome either way.
        onAccepted: root.bridge.exportProfile(exportDialog.profileId, settingsController.urlToLocalFile(selectedFile))
    }

    FileDialog {
        id: importDialog

        title: i18n("Import Profile")
        nameFilters: [i18n("PlasmaZones Profile (*.json)"), i18n("All files (*)")]
        fileMode: FileDialog.OpenFile
        onAccepted: {
            // Import BY PATH, report BY URL: showResult percent-decodes its
            // argument, and a dialog path is already decoded.
            const path = settingsController.urlToLocalFile(selectedFile);
            importCard.showResult(root._importProfile(path), selectedFile);
        }
    }
}
