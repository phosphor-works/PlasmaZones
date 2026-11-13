// SPDX-FileCopyrightText: 2026 fuddlesworth
// SPDX-License-Identifier: GPL-3.0-or-later

import QtQuick
import QtQuick.Controls
import QtQuick.Layouts
import org.kde.kirigami as Kirigami

/**
 * @brief A reusable shortcut capture TextField for Kirigami.FormLayout.
 *
 * Click the field, or focus it and press Enter, Return or Space, to enter
 * capture mode, then press a key combination. Escape cancels. Emits
 * keySequenceModified when a valid sequence is captured, or with an empty
 * string when a clearable field is cleared.
 *
 * The caller owns keySequence: the field never writes it, so a capture
 * only sticks when the keySequenceModified handler persists the value
 * and the caller propagates the change back into keySequence (binding
 * or imperative re-sync). capturing is already false when the signal
 * fires, so the field has left its capture prompt by then.
 *
 * A recorded chord is written in the canonical spelling
 * (SettingsController::canonicalShortcut: QKeySequence PortableText, Meta,
 * Ctrl, Alt, Shift), the spelling the daemon registers and every chord
 * compare uses. Shift is kept only where it is a real modifier: on a key
 * that types a shifted symbol (Shift+1 types "!") it is dropped and the
 * symbol recorded, which is the rule KDE's own recorder applies and the form
 * KWin looks a global shortcut up by.
 *
 * While capturing, the field holds a session-wide block on global shortcuts
 * (SettingsController::setGlobalShortcutsBlocked), so a chord some other
 * global action already holds reaches the field instead of firing. The block
 * is released on every exit: a capture, Escape, focus loss, a hidden or
 * destroyed field. Where the platform offers no such block, the field says
 * that a chord already in use elsewhere cannot be recorded.
 */
TextField {
    id: root

    property string keySequence
    property string tooltipText
    property bool capturing: false
    //* Name announced to assistive technology; falls back to the field text.
    property string accessibleName: ""
    /// Refuse a key pressed with no modifier. Set for every chord the daemon
    /// grabs globally, where a bare key would be taken from every application.
    property bool requireModifier: false
    /// Offer a clear button (and Delete/Backspace while focused) that emits
    /// keySequenceModified("").
    property bool clearable: false
    /// Check a captured chord against every other PlasmaZones global chord
    /// (SettingsController::workspaceShortcutConflict) and refuse a taken one.
    property bool checkConflicts: false
    /// The chord's own key for the conflict check: the Settings property name,
    /// or SettingsController::namedEntryShortcutKey for a named workspace's
    /// chord. Empty for a chord with no stored owner yet.
    property string conflictKey: ""

    /// Why the last captured chord was refused, shown while the field holds
    /// focus. Cleared on the next capture attempt and on focus loss.
    property string message: ""

    // Whether this field currently holds the global-shortcut block.
    property bool _blockHeld: false

    signal keySequenceModified(string sequence)

    function _beginCapture() {
        if (root.capturing)
            return;
        // Focus FIRST: a sibling field that is still capturing releases its
        // block in its own focus-loss handler, and it must do that before this
        // field takes the block, or the release would lift it for this one.
        root.forceActiveFocus(Qt.OtherFocusReason);
        root.message = "";
        root.capturing = true;
        if (settingsController.canBlockGlobalShortcuts()) {
            root._blockHeld = settingsController.setGlobalShortcutsBlocked(true);
            if (!root._blockHeld)
                root.message = i18n("Shortcuts already in use elsewhere cannot be recorded here.");
        } else {
            root.message = i18n("Shortcuts already in use elsewhere cannot be recorded here.");
        }
    }

    function _releaseBlock() {
        if (!root._blockHeld)
            return;
        root._blockHeld = false;
        settingsController.setGlobalShortcutsBlocked(false);
    }

    // Whether Shift is a real modifier on this key, as KDE's recorder decides
    // it (KKeySequenceRecorder's isShiftAsModifierAllowed): letters, function
    // keys and keys that type nothing. On a key that types a symbol, Shift is
    // what produced the symbol, and the chord is recorded without it.
    function _shiftIsModifier(key) {
        if (key >= Qt.Key_F1 && key <= Qt.Key_F35)
            return true;
        if (key > 0 && key <= 0xffff && /\p{L}/u.test(String.fromCharCode(key)))
            return true;
        switch (key) {
        case Qt.Key_Return:
        case Qt.Key_Enter:
        case Qt.Key_Space:
        case Qt.Key_Backspace:
        case Qt.Key_Tab:
        case Qt.Key_Backtab:
        case Qt.Key_Escape:
        case Qt.Key_Print:
        case Qt.Key_ScrollLock:
        case Qt.Key_Pause:
        case Qt.Key_PageUp:
        case Qt.Key_PageDown:
        case Qt.Key_Insert:
        case Qt.Key_Delete:
        case Qt.Key_Home:
        case Qt.Key_End:
        case Qt.Key_Up:
        case Qt.Key_Down:
        case Qt.Key_Left:
        case Qt.Key_Right:
        case Qt.Key_Menu:
            return true;
        default:
            return false;
        }
    }

    // The key's PortableText name, or "" for a key this field cannot spell.
    // Names are QKeySequence's own, so canonicalShortcut parses them back.
    function _keyName(key) {
        if (key >= Qt.Key_A && key <= Qt.Key_Z)
            return String.fromCharCode('A'.charCodeAt(0) + (key - Qt.Key_A));
        if (key >= Qt.Key_0 && key <= Qt.Key_9)
            return String.fromCharCode('0'.charCodeAt(0) + (key - Qt.Key_0));
        if (key >= Qt.Key_F1 && key <= Qt.Key_F35)
            return "F" + (key - Qt.Key_F1 + 1);
        switch (key) {
        case Qt.Key_Return:
            return "Return";
        case Qt.Key_Enter:
            return "Enter";
        case Qt.Key_Space:
            return "Space";
        case Qt.Key_Tab:
        case Qt.Key_Backtab:
            return "Tab";
        case Qt.Key_Backspace:
            return "Backspace";
        case Qt.Key_Escape:
            return "Esc";
        case Qt.Key_Delete:
            return "Del";
        case Qt.Key_Insert:
            return "Ins";
        case Qt.Key_Left:
            return "Left";
        case Qt.Key_Right:
            return "Right";
        case Qt.Key_Up:
            return "Up";
        case Qt.Key_Down:
            return "Down";
        case Qt.Key_Home:
            return "Home";
        case Qt.Key_End:
            return "End";
        case Qt.Key_PageUp:
            return "PgUp";
        case Qt.Key_PageDown:
            return "PgDown";
        case Qt.Key_Print:
            return "Print";
        case Qt.Key_Pause:
            return "Pause";
        case Qt.Key_ScrollLock:
            return "ScrollLock";
        case Qt.Key_Menu:
            return "Menu";
        default:
            // Printable ASCII and Latin-1 symbols are their own name.
            if (key > 0x20 && key < 0x100)
                return String.fromCharCode(key);
            return "";
        }
    }

    // The modifiers that count for the chord: Shift is dropped where it only
    // produced the key's symbol (see _shiftIsModifier).
    function _chordModifiers(key, modifiers) {
        var mods = modifiers & (Qt.MetaModifier | Qt.ControlModifier | Qt.AltModifier | Qt.ShiftModifier);
        if ((mods & Qt.ShiftModifier) && !root._shiftIsModifier(key))
            mods &= ~Qt.ShiftModifier;
        return mods;
    }

    // The chord in Meta, Ctrl, Alt, Shift order, or "" when the key has no
    // name this field can spell. `mods` comes from _chordModifiers.
    function _buildSequence(key, mods) {
        var keyStr = root._keyName(key);
        if (keyStr.length === 0)
            return "";
        var parts = [];
        if (mods & Qt.MetaModifier)
            parts.push("Meta");
        if (mods & Qt.ControlModifier)
            parts.push("Ctrl");
        if (mods & Qt.AltModifier)
            parts.push("Alt");
        if (mods & Qt.ShiftModifier)
            parts.push("Shift");
        parts.push(keyStr);
        return parts.join("+");
    }

    // Judge a captured chord. Returns the refusal, or "" to accept.
    function _refusalFor(canonical, hasModifier) {
        if (canonical === "")
            return i18n("That key cannot be used in a shortcut.");
        if (root.requireModifier && !hasModifier)
            return i18n("A global shortcut needs a modifier key such as Meta or Ctrl.");
        if (root.checkConflicts) {
            var conflict = settingsController.workspaceShortcutConflict(canonical, root.conflictKey);
            if (conflict.conflict)
                return i18nc("%1 names the action that already has the shortcut", "This shortcut is already assigned to %1.", conflict.label);
        }
        return "";
    }

    Accessible.name: accessibleName !== "" ? accessibleName : text
    Accessible.description: {
        if (root.message !== "")
            return root.message;
        if (root.capturing)
            return i18n("Press the new shortcut, or Escape to cancel.");
        return root.clearable ? i18n("Press Enter or Space to record a new shortcut, or Delete to clear it.") : i18n("Press Enter or Space to record a new shortcut.");
    }
    Layout.fillWidth: true
    text: capturing ? i18n("Press keys…") : (keySequence || "")
    placeholderText: i18nc("@info:placeholder example keyboard shortcut", "Ctrl+Shift+X")
    readOnly: true
    color: capturing ? Kirigami.Theme.highlightColor : Kirigami.Theme.textColor
    rightPadding: clearButton.visible ? clearButton.width + Kirigami.Units.smallSpacing : leftPadding
    Keys.priority: Keys.BeforeItem
    // A window Shortcut (the settings app's Ctrl+PgUp/PgDown page stepping,
    // for one) resolves before Keys.onPressed ever runs. Claiming the override
    // while capturing hands every chord to the recorder instead.
    Keys.onShortcutOverride: event => {
        if (root.capturing)
            event.accepted = true;
    }
    Keys.onPressed: event => {
        if (!root.capturing) {
            if ((event.modifiers & ~Qt.KeypadModifier) !== Qt.NoModifier)
                return;
            if (event.key === Qt.Key_Return || event.key === Qt.Key_Enter || event.key === Qt.Key_Space) {
                event.accepted = true;
                root._beginCapture();
            } else if (root.clearable && (event.key === Qt.Key_Delete || event.key === Qt.Key_Backspace) && root.keySequence !== "") {
                event.accepted = true;
                root.keySequenceModified("");
            }
            return;
        }

        event.accepted = true;
        if (event.key === Qt.Key_Escape && event.modifiers === Qt.NoModifier) {
            root.message = "";
            root.capturing = false;
            return;
        }
        var key = event.key;
        if (key === Qt.Key_Shift || key === Qt.Key_Control || key === Qt.Key_Alt || key === Qt.Key_AltGr || key === Qt.Key_Meta || key === Qt.Key_Super_L || key === Qt.Key_Super_R)
            return;

        var mods = root._chordModifiers(key, event.modifiers);
        var built = root._buildSequence(key, mods);
        if (built.length === 0)
            return;
        var canonical = settingsController.canonicalShortcut(built);
        var refusal = root._refusalFor(canonical, mods !== Qt.NoModifier);
        if (refusal !== "") {
            // Stay in capture mode so the next attempt needs no extra step.
            root.message = refusal;
            return;
        }
        // Leave capture mode before emitting, so `text` (bound to
        // `capturing ? … : keySequence`) has left the capture prompt and
        // follows keySequence again by the time a handler writes the new
        // value back.
        // Never self-assign keySequence here: that would sever the
        // caller's binding on first capture.
        root.message = "";
        root.capturing = false;
        root.keySequenceModified(canonical);
    }
    onCapturingChanged: {
        if (!capturing)
            root._releaseBlock();
    }
    onActiveFocusChanged: {
        if (!activeFocus) {
            root.capturing = false;
            root.message = "";
        }
    }
    // A field hidden mid-capture loses focus too, but a disabled one need not.
    onEnabledChanged: {
        if (!enabled)
            root.capturing = false;
    }
    Component.onDestruction: root._releaseBlock()

    ToolTip.visible: (root.message !== "" && root.activeFocus) || (hovered && !capturing && tooltipText.length > 0)
    ToolTip.text: root.message !== "" ? root.message : tooltipText
    ToolTip.delay: root.message !== "" ? 0 : Kirigami.Units.toolTipDelay

    MouseArea {
        anchors.fill: parent
        acceptedButtons: Qt.LeftButton
        cursorShape: Qt.PointingHandCursor
        onClicked: root._beginCapture()
    }

    // Declared after the MouseArea so it stacks above it and gets its own
    // clicks.
    ToolButton {
        id: clearButton

        anchors.right: parent.right
        anchors.verticalCenter: parent.verticalCenter
        height: parent.height - Kirigami.Units.smallSpacing
        width: height
        visible: root.clearable && root.keySequence !== "" && !root.capturing
        icon.name: "edit-clear"
        focusPolicy: Qt.NoFocus
        Accessible.name: root.accessibleName !== "" ? i18nc("@action:button %1 names the shortcut", "Clear %1", root.accessibleName) : i18nc("@action:button", "Clear shortcut")
        ToolTip.text: i18nc("@info:tooltip", "Clear shortcut")
        ToolTip.visible: hovered
        ToolTip.delay: Kirigami.Units.toolTipDelay
        onClicked: root.keySequenceModified("")
    }
}
