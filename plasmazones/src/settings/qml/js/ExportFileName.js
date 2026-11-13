// SPDX-FileCopyrightText: 2026 fuddlesworth
// SPDX-License-Identifier: GPL-3.0-or-later

.pragma library

// Drops the characters forDisplayName documents as refused.
function _strip(text) {
    return String(text || "").replace(/[\/\\:*?"<>|#%]/g, "").trim();
}

/**
 * The file name an export's save dialog opens with: "<display name>.<suffix>",
 * so the user does not start from an empty field.
 *
 * Characters a file system refuses are stripped rather than escaped, and so
 * are the URL-significant ones (#, %): the host splices the result into a
 * file URL (`dialog.currentFolder + "/" + name`) and reads it back through
 * urlToLocalFile, so an unescaped # would parse as a fragment and silently
 * cut the name short. Everything else, non-ASCII letters included, is kept,
 * because the name is the user's own.
 *
 * @p fallback names the file when nothing usable is left. It goes through the
 * same strip, because a caller may pass a translated word and a translation
 * can carry any of those characters too.
 */
function forDisplayName(displayName, suffix, fallback) {
    const name = _strip(displayName);
    return (name.length > 0 ? name : _strip(fallback)) + "." + suffix;
}
