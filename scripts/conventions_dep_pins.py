# SPDX-FileCopyrightText: 2026 Phosphor Works Team
# SPDX-License-Identifier: GPL-3.0-or-later
"""The dep-pins rule: every packaging recipe carries the archive pins exactly.

Two files pin the archives a PlasmaZones build needs (version, URL, file name,
SHA-256). cmake/PlasmaZonesDependencies.cmake pins PlasmaZones' own: the
phosphor-libs commit it builds against and the branding release. That phosphor-libs
commit's cmake/PhosphorDependencies.cmake pins the library tree's third-party
sources (Luau, jsoncons), and this rule reads it from the pinned tree itself
(scripts/pinned_phosphor_libs.py), so a phosphor-libs bump that moves one of those
pins is checked too. With PHOSPHOR_LIBS_DIR set, that tree is the checkout it names
instead, and the messages say so rather than naming the pinned commit. An offline
packaging build cannot download, so each recipe in PIN_RECIPES (in this repository)
and PACKAGING_PIN_RECIPES (in plasmazones/ of the packaging repository, see
conventions_packaging.py) lists every one of these archives as an extra source of its
own, and in doing so repeats the URL and the hash. Nothing else ties those copies to the pins, so a bump that missed one recipe
would only surface when that distro's build failed its hash check, or worse, built
from whatever the recipe still named. This rule fails the gate instead, naming the
recipe and the dependency. It also fails a recipe that still fetches an archive no
pin names any more, which is what a bump that replaces one dependency with another
leaves behind: every literal https URL of a .tar.gz in a recipe has to be some pin's
URL. The recipes' own release tarball is spelled through a variable (`$url`,
`%{url}`, fetchFromGitHub), so it is not such a literal.

The jobs that stage the archives with the fetch script (the Debian and OBS paths in
plasmazones-release.yml of the packaging repository) read the pin files themselves and
carry no copy, so they are not listed.

Stdlib only, and it returns plain tuples rather than the gate's Violation so the two
files cannot form a cycle, the same shape as conventions_shared_text.py.
"""
from __future__ import annotations

import base64
import os
import re
from pathlib import Path
from typing import Callable

from conventions_packaging import LABEL as PACKAGING_LABEL

PIN_FILE = "cmake/PlasmaZonesDependencies.cmake"
# The library tree's own pin file, relative to that tree.
LIBS_PIN_FILE = "cmake/PhosphorDependencies.cmake"
# The PlasmaZones pin naming the phosphor-libs commit, whose own pins are checked too.
LIBS_DEP = "phosphor-libs"
# The environment variable scripts/pinned_phosphor_libs.py takes a local tree from.
LIBS_OVERRIDE_ENV = "PHOSPHOR_LIBS_DIR"

# Every recipe that fetches the archives itself and so must repeat each pin: the Nix
# recipe in this repository, beside its flake, and the others in plasmazones/ of the
# packaging repository.
PIN_RECIPES = (
    "nix/package.nix",
)
PACKAGING_PIN_RECIPES = (
    "arch/PKGBUILD",
    "arch/PKGBUILD.git",
    "rpm/plasmazones.spec",
)

# The variable prefix each pin file uses: `set(<PREFIX>S a b)` lists the pins and
# `set(<PREFIX>_<name>_<FIELD> "value")` holds each field, one per line. Held strict on
# purpose: CI's luau-analyze job reads the Luau pin with a sed of the same shape, so a
# reformatted pin that this regex stops matching would also break that job, and this
# rule says so first.
APP_PREFIX = "PLASMAZONES_DEP"
LIBS_PREFIX = "PHOSPHOR_DEP"
_HEX64 = re.compile(r"^[0-9a-f]{64}$")
# A literal archive URL in a recipe. Stops at the quote, whitespace or `#/` fragment
# that ends one in the PKGBUILD, spec and Nix spellings.
_ARCHIVE_URL = re.compile(r"""https://[^\s"'#)]+?\.tar\.gz""")


def read_pins(text: str, prefix: str = APP_PREFIX) -> tuple[dict[str, dict[str, str]], list[str]]:
    """Parse a pin file. Returns ({dep: {field: value}}, [problem])."""
    problems: list[str] = []
    m = re.search(rf"^set\({prefix}S ([^)]*)\)\s*$", text, re.M)
    if not m:
        return {}, [f"no `set({prefix}S ...)` line, so the rule cannot tell which archives are pinned"]
    names = m.group(1).split()
    if not names:
        problems.append(f"{prefix}S is empty, so this rule checks nothing")
    fields: dict[str, dict[str, str]] = {n: {} for n in names}
    for dep, field, value in re.findall(
            rf'^set\({prefix}_([A-Za-z0-9_-]+?)_(VERSION|URL|FILE|SHA256) "([^"]*)"\)\s*$', text, re.M):
        if dep in fields:
            fields[dep][field] = value
    for dep, got in fields.items():
        missing = [f for f in ("VERSION", "URL", "FILE", "SHA256") if f not in got]
        if missing:
            problems.append(f"{dep} is in {prefix}S but has no {', '.join(missing)} pin "
                            f"in the one-line `set({prefix}_{dep}_<FIELD> \"...\")` shape")
        elif not _HEX64.match(got["SHA256"]):
            problems.append(f"{dep}'s SHA256 pin is not 64 lowercase hex digits")
    return fields, problems


def _pinned_tree(repo: Path) -> Path:
    from pinned_phosphor_libs import tree

    return tree(repo)


def _libs_label(root: Path, pinned_version: str) -> str:
    """Name the library tree whose pin file was read, for the messages.

    The pinned commit names it only when that is the tree read. A tree that
    PHOSPHOR_LIBS_DIR put in its place is named by its path, with the commit it
    stands in for, so a finding reported against a local checkout is not passed
    off as one about the commit this repository pins.
    """
    override = os.environ.get(LIBS_OVERRIDE_ENV)
    if override and Path(override).resolve() == root.resolve():
        return (f"{LIBS_DEP} at {root} ({LIBS_OVERRIDE_ENV}, in place of the pinned "
                f"{pinned_version}): {LIBS_PIN_FILE}")
    return f"{LIBS_DEP} {pinned_version}: {LIBS_PIN_FILE}"


def dep_pin_problems(repo: Path,
                     libs_tree: Callable[[Path], Path] = _pinned_tree,
                     packaging: Path | None = None) -> list[tuple[str, str]]:
    """Return (path, message) for each recipe that drifted from a pin.

    `repo` is passed in and every path is anchored on it, never on the process CWD,
    so the rule cannot pass in silence when run from another directory. `libs_tree`
    finds the pinned phosphor-libs tree, and is only called when the pin file pins
    one. `packaging` is plasmazones/ of the packaging checkout. Without it only the
    recipes of this repository are checked, and the caller reports why.
    """
    out: list[tuple[str, str]] = []
    pin_path = repo / PIN_FILE
    try:
        text = pin_path.read_text(encoding="utf-8")
    except OSError as e:
        return [(PIN_FILE, f"cannot read the pin file ({e.strerror}), so no recipe was checked")]
    pins, problems = read_pins(text, APP_PREFIX)
    out.extend((PIN_FILE, p) for p in problems)
    # Where each pin was read, for the messages: a stale Luau hash is fixed by
    # copying it from the library tree's file, not from this repository's.
    source = {dep: PIN_FILE for dep in pins}
    # Whether every pin was read, so the URLs they name are the whole set of
    # archives a recipe may fetch. Without that, the check for an archive no pin
    # names would report every archive of a pin file it could not read, under a
    # cause that is not the real one, which is already reported.
    all_pins_read = not problems
    if LIBS_DEP in pins:
        try:
            root = libs_tree(repo)
            libs_text = (root / LIBS_PIN_FILE).read_text(encoding="utf-8")
        except (OSError, RuntimeError) as e:
            which = (f"the {LIBS_DEP} tree {LIBS_OVERRIDE_ENV} names" if os.environ.get(LIBS_OVERRIDE_ENV)
                     else f"the pinned {LIBS_DEP} tree")
            out.append((PIN_FILE, f"cannot read {LIBS_PIN_FILE} of {which} ({e}), "
                                  f"so its pins were not checked"))
            all_pins_read = False
        else:
            libs_pins, libs_problems = read_pins(libs_text, LIBS_PREFIX)
            all_pins_read = all_pins_read and not libs_problems
            label = _libs_label(root, pins[LIBS_DEP].get("VERSION", "?"))
            out.extend((label, p) for p in libs_problems)
            for dep, got in libs_pins.items():
                if dep in pins:
                    out.append((label, f"pins {dep}, which {PIN_FILE} pins too; one of the two has to go"))
                    continue
                pins[dep] = got
                source[dep] = label
    recipes = [(r, repo / r) for r in PIN_RECIPES]
    if packaging is not None:
        recipes += [(f"{PACKAGING_LABEL}/{r}", packaging / r) for r in PACKAGING_PIN_RECIPES]
    # An empty recipe list is the quiet way to switch the rule off while it stays in
    # --list-rules, so it has to announce itself.
    if not recipes:
        out.append(("scripts/conventions_dep_pins.py",
                    "PIN_RECIPES is empty, so this rule checks nothing; delete the rule outright "
                    "rather than emptying its list"))
    for recipe, path in recipes:
        try:
            body = path.read_text(encoding="utf-8")
        except OSError as e:
            out.append((recipe, f"listed in PIN_RECIPES but cannot be read ({e.strerror})"))
            continue
        for dep, got in pins.items():
            if "URL" not in got or "SHA256" not in got:
                continue
            if got["URL"] not in body:
                out.append((recipe, f"does not fetch {dep} from its pinned URL {got['URL']} "
                                    f"({source[dep]})"))
            if got["SHA256"] not in body:
                out.append((recipe, f"does not carry {dep}'s pinned SHA-256 {got['SHA256']} "
                                    f"({source[dep]})"))
            # Nix verifies the SRI form, not the hex one the rule finds in its comment,
            # so a .nix recipe has to carry both and they must be the same digest.
            if recipe.endswith(".nix") and _HEX64.match(got["SHA256"]):
                sri = "sha256-" + base64.b64encode(bytes.fromhex(got["SHA256"])).decode()
                if sri not in body:
                    out.append((recipe, f"does not carry {dep}'s pinned hash in the SRI form Nix "
                                        f"checks ({sri})"))
        if all_pins_read:
            pinned_urls = {got["URL"] for got in pins.values()}
            for url in sorted(set(_ARCHIVE_URL.findall(body)) - pinned_urls):
                out.append((recipe, f"fetches {url}, which no pin names; remove it, or pin it in "
                                    f"{PIN_FILE}"))
    return out
