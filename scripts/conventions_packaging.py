# SPDX-FileCopyrightText: 2026 Phosphor Works Team
# SPDX-License-Identifier: GPL-3.0-or-later
"""Where the release recipes of PlasmaZones are, for the rules that read them.

The Arch, Debian, RPM and OBS recipes are in plasmazones/ of the
phosphor-works/packaging repository, not in this one. Three rules of
check-conventions.py read them: dep5 (debian/copyright declares the license of
each file of this tree), dep-pins (the recipes repeat the pins of
cmake/PlasmaZonesDependencies.cmake) and recipe-prose (the package
descriptions). The Nix recipe stays in this repository, beside its flake.

The rules find the packaging checkout at PLASMAZONES_PACKAGING_DIR when it is
set, and else at ../packaging beside this checkout, which is where the workspace
clones it. CI checks out phosphor-works/packaging and sets the variable. When no
checkout is there, a whole-tree run reports it as a finding, so a check that
cannot read the recipes never passes. A pre-commit run prints a note and skips
those rules, because a commit to this repository alone does not change the
recipes. CI and the packaging repository's own CI check them both ways.

Stdlib only, and nothing here imports the gate, the same shape as the other
conventions_*.py siblings.
"""
from __future__ import annotations

import os
import subprocess
from pathlib import Path

# The environment variable that names a checkout of phosphor-works/packaging.
ENV = "PLASMAZONES_PACKAGING_DIR"
# The directory of that repository that holds the PlasmaZones recipes.
SUBDIR = "plasmazones"
# How the messages name a file there: packaging/plasmazones/debian/copyright.
LABEL = f"packaging/{SUBDIR}"


def checkout(repo: Path) -> Path:
    """The packaging checkout the rules read: $PLASMAZONES_PACKAGING_DIR, or ../packaging."""
    named = os.environ.get(ENV)
    return Path(named) if named else repo.parent / "packaging"


def recipes_dir(repo: Path) -> Path | None:
    """plasmazones/ of the packaging checkout, or None when it is not there."""
    d = checkout(repo) / SUBDIR
    return d if d.is_dir() else None


def missing_message(repo: Path) -> str:
    return (f"cannot find the PlasmaZones recipes at {checkout(repo) / SUBDIR}, so they were not "
            f"checked. Clone https://github.com/phosphor-works/packaging beside this checkout, "
            f"or set {ENV} to a checkout of it")


def tracked_recipes(root: Path) -> list[str]:
    """Every tracked file of plasmazones/ in the packaging checkout, relative to plasmazones/.

    Raises OSError or subprocess.CalledProcessError when git cannot list them, which the
    caller reports: a listing that came back empty would check nothing in silence.

    git runs without the GIT_* variables of the caller. A pre-commit hook of this
    repository sets GIT_INDEX_FILE and others, and git would read this repository's
    index for the other checkout."""
    env = {k: v for k, v in os.environ.items() if not k.startswith("GIT_")}
    out = subprocess.run(["git", "ls-files", "-z", "--", "."], cwd=root, capture_output=True,
                         check=True, env=env).stdout.decode("utf-8", errors="replace")
    return sorted(p for p in out.split("\0") if p)
