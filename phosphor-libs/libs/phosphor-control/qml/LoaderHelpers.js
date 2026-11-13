.pragma library

// SPDX-FileCopyrightText: 2026 fuddlesworth
// SPDX-License-Identifier: LGPL-2.1-or-later

// Shared Loader.onLoaded utilities for the chrome's Loaders that mount
// content from elsewhere: consumer-supplied Components, pages and module
// sections. Centralised so the subtle "imperative Qt.binding assignment
// overrides any consumer Layout.fillWidth on the loaded item" pitfall has
// one place to document it.
//
// Usage from inside a Loader's onLoaded handler:
//   onLoaded: PhosphorLoaderHelpers.bindItemWidthToLoader(loaderId)

/**
 * Bind the loaded item's `width` to the Loader's own width.
 *
 * Why imperative `item.width = Qt.binding(...)` rather than a
 * declarative `binding`? The loaded item is created at runtime by
 * `sourceComponent`/`source`, so a declarative binding inside the
 * Loader cannot directly target the not-yet-instantiated child.
 * `onLoaded` runs once the item exists; assigning a Qt.binding from
 * there installs a real binding (re-evaluates whenever loader.width
 * changes) rather than a one-shot copy.
 *
 * KNOWN CONSUMER CONTRACT: consumer Components whose root sets
 * `Layout.fillWidth: true` will see this imperative assignment
 * override that — the loader's width wins. If a consumer needs a
 * narrower slot than the loader, they must compute their preferred
 * width inside their Component and the loader's parent layout has
 * to size the loader accordingly upstream.
 */
function bindItemWidthToLoader(loader) {
    if (loader && loader.item)
        loader.item.width = Qt.binding(function () {
            return loader.width;
        });
}

/**
 * Inject `value` into `item[propName]` only when the item declares
 * that property. Centralises the `hasOwnProperty + try-catch` idiom the
 * chrome uses to forward a controller, settingsApp or compact flag into
 * loaded content.
 *
 * A property the item declares read-only (it supplies its own value, as
 * a page binding `readonly property var controller` to its own controller
 * does) is left alone silently: QML throws a TypeError for that write,
 * which is the one failure this helper exists to tolerate. Any other
 * failure, such as a value of the wrong type for the declared property,
 * is a bug in the item or the caller and is logged.
 *
 * `value` may be `null`: callers pass through whatever they hold, and
 * PageHost's onLoaded passes the registry's controller, which is null
 * once that controller has been destroyed.
 */
function injectIfAssignable(item, propName, value) {
    if (!item || !item.hasOwnProperty(propName))
        return;
    try {
        item[propName] = value;
    } catch (e) {
        if (e instanceof TypeError && String(e.message).startsWith("Cannot assign to read-only property"))
            return;
        console.warn("injectIfAssignable: could not set", propName, "on", item, ":", e);
    }
}
