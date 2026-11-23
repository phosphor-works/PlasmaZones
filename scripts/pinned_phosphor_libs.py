#!/usr/bin/env python3
# SPDX-FileCopyrightText: 2026 fuddlesworth
# SPDX-License-Identifier: GPL-3.0-or-later
"""Find the phosphor-libs tree this checkout builds against.

PlasmaZones builds the Phosphor libraries from the phosphor-libs commit
cmake/PlasmaZonesDependencies.cmake pins. The checks that read files of that
tree without a configured build (the JSON schema gate, the dep-pins rule, CI's
luau-analyze job) find it here, so they judge this checkout against the same
commit its build uses.

In order:

1. PHOSPHOR_LIBS_DIR in the environment names a phosphor-libs tree, used as it
   is. That is the counterpart of CMake's -DCPM_phosphor-libs_SOURCE=<dir>, for
   checking against a local checkout before its commit is pinned.
2. Otherwise the pinned archive, checked against its SHA-256 and unpacked once
   into .cache/phosphor-libs/<commit>/ at the repository root. The archive is
   taken from PHOSPHOR_DEPS_ARCHIVES when that names a directory holding it (the
   same directory an offline CMake build is handed), and downloaded otherwise.

Run as a script it prints the tree's path, for shell callers:

    tree=$(python3 scripts/pinned_phosphor_libs.py)

engine_tree() finds the phosphor-engine-libs tree the same way, with
PHOSPHOR_ENGINE_LIBS_DIR as its override, and pinned_tree() does the same for
any archive the pin file pins: scripts/pinned_surface_packs.py finds the
surface packs with it.

Stdlib only.
"""
from __future__ import annotations

import hashlib
import os
import re
import shutil
import sys
import tarfile
import tempfile
import urllib.request
from pathlib import Path

PIN_FILE = "cmake/PlasmaZonesDependencies.cmake"
LIBS_DEP = "phosphor-libs"
ENGINE_LIBS_DEP = "phosphor-engine-libs"


class PinnedTreeError(RuntimeError):
    """The pinned tree could not be found, fetched or trusted."""


def read_pin(repo: Path, dep: str = LIBS_DEP) -> dict[str, str]:
    """The VERSION, URL, FILE and SHA256 fields of the pin named `dep`."""
    try:
        text = (repo / PIN_FILE).read_text(encoding="utf-8")
    except OSError as e:
        raise PinnedTreeError(f"cannot read {PIN_FILE} ({e.strerror})") from e
    field = re.compile(rf'^set\(PLASMAZONES_DEP_{re.escape(dep)}_(VERSION|URL|FILE|SHA256) "([^"]*)"\)\s*$', re.M)
    pin = dict(field.findall(text))
    missing = [f for f in ("VERSION", "URL", "FILE", "SHA256") if f not in pin]
    if missing:
        raise PinnedTreeError(f"{PIN_FILE} has no {dep} {', '.join(missing)} pin")
    return pin


def _sha256(path: Path) -> str:
    h = hashlib.sha256()
    with path.open("rb") as f:
        for block in iter(lambda: f.read(1 << 20), b""):
            h.update(block)
    return h.hexdigest()


def _archive(pin: dict[str, str], cache: Path, override_env: str) -> Path:
    offline = os.environ.get("PHOSPHOR_DEPS_ARCHIVES")
    if offline and (Path(offline) / pin["FILE"]).is_file():
        path = Path(offline) / pin["FILE"]
    else:
        path = cache / pin["FILE"]
        if not path.is_file() or _sha256(path) != pin["SHA256"]:
            part = path.with_name(path.name + ".part")
            try:
                with urllib.request.urlopen(pin["URL"], timeout=60) as r, part.open("wb") as out:
                    shutil.copyfileobj(r, out)
            except OSError as e:
                part.unlink(missing_ok=True)
                raise PinnedTreeError(
                    f"cannot download {pin['URL']} ({e}). Set {override_env} to a local tree, "
                    f"or PHOSPHOR_DEPS_ARCHIVES to a directory holding {pin['FILE']}") from e
            part.replace(path)
    got = _sha256(path)
    if got != pin["SHA256"]:
        raise PinnedTreeError(f"{path} does not match its pin:\n  expected {pin['SHA256']}\n  got      {got}")
    return path


def pinned_tree(repo: Path, dep: str, override_env: str) -> Path:
    """The unpacked tree of the archive pinned as `dep`, for the checkout at `repo`.

    The archive is unpacked once into .cache/<dep>/<version>/. The caller handles
    `override_env`, the environment variable that names a local tree instead,
    because what that tree has to look like differs per dependency; it is only
    named here in the error a failed download gives.
    """
    pin = read_pin(repo, dep)
    cache = repo / ".cache" / dep
    dest = cache / pin["VERSION"]
    # Written after the extraction finishes, so one cut short is redone rather
    # than passing as complete. The hash is part of it, so a re-pinned archive
    # under the same commit is unpacked again.
    stamp = cache / f".{pin['VERSION']}-{pin['SHA256']}"
    if stamp.is_file() and dest.is_dir():
        return dest
    cache.mkdir(parents=True, exist_ok=True)
    archive = _archive(pin, cache, override_env)
    shutil.rmtree(dest, ignore_errors=True)
    with tempfile.TemporaryDirectory(dir=cache) as tmp:
        with tarfile.open(archive) as tar:
            tar.extractall(tmp, filter="data")
        top = list(Path(tmp).iterdir())
        src = top[0] if len(top) == 1 and top[0].is_dir() else Path(tmp)
        shutil.move(str(src), dest)
    stamp.touch()
    return dest


def tree(repo: Path) -> Path:
    """The phosphor-libs tree for the checkout at `repo`."""
    override = os.environ.get("PHOSPHOR_LIBS_DIR")
    if override:
        root = Path(override).resolve()
        if not (root / "cmake" / "PhosphorDependencies.cmake").is_file():
            raise PinnedTreeError(f"PHOSPHOR_LIBS_DIR is {override}, which is not a phosphor-libs tree")
        return root
    return pinned_tree(repo, LIBS_DEP, "PHOSPHOR_LIBS_DIR")


def engine_tree(repo: Path) -> Path:
    """The phosphor-engine-libs tree for the checkout at `repo`."""
    override = os.environ.get("PHOSPHOR_ENGINE_LIBS_DIR")
    if override:
        root = Path(override).resolve()
        if not (root / "cmake" / "PhosphorEngineLibsDependencies.cmake").is_file():
            raise PinnedTreeError(f"PHOSPHOR_ENGINE_LIBS_DIR is {override}, which is not a phosphor-engine-libs tree")
        return root
    return pinned_tree(repo, ENGINE_LIBS_DEP, "PHOSPHOR_ENGINE_LIBS_DIR")


def main() -> int:
    repo = Path(__file__).resolve().parent.parent
    try:
        print(tree(repo))
    except PinnedTreeError as e:
        print(f"pinned_phosphor_libs: {e}", file=sys.stderr)
        return 1
    return 0


if __name__ == "__main__":
    sys.exit(main())
