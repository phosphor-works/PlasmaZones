#!/usr/bin/env python3
# SPDX-FileCopyrightText: 2026 Phosphor Works Team
# SPDX-License-Identifier: GPL-3.0-or-later
"""Find the surface decoration packs this checkout installs.

The surface packs live in phosphor-works/phosphor-surface-packs, at the commit
cmake/PlasmaZonesDependencies.cmake pins. The checks that read them without a
configured build (the JSON schema gate and the shared-param-text rule) find
them here, the way scripts/pinned_phosphor_libs.py finds the library tree.

In order:

1. PHOSPHOR_SURFACE_PACKS_DIR in the environment names a packs directory (the
   packs/ directory of a phosphor-surface-packs checkout), used as it is. It
   means what the CMake variable of that name means.
2. Otherwise the pinned archive, checked against its SHA-256 and unpacked once
   into .cache/phosphor-surface-packs/<commit>/ at the repository root, taken
   from PHOSPHOR_DEPS_ARCHIVES when that holds it and downloaded otherwise.

Run as a script it prints the packs directory, for shell callers:

    packs=$(python3 scripts/pinned_surface_packs.py)

Stdlib only.
"""
from __future__ import annotations

import os
import sys
from pathlib import Path

from pinned_phosphor_libs import PinnedTreeError, pinned_tree

DEP = "phosphor-surface-packs"
OVERRIDE_ENV = "PHOSPHOR_SURFACE_PACKS_DIR"

__all__ = ["DEP", "OVERRIDE_ENV", "PinnedTreeError", "tree"]


def tree(repo: Path) -> Path:
    """The packs directory for the checkout at `repo`."""
    override = os.environ.get(OVERRIDE_ENV)
    if override:
        packs = Path(override).resolve()
    else:
        packs = pinned_tree(repo, DEP, OVERRIDE_ENV) / "packs"
    if not (packs / "shared").is_dir():
        raise PinnedTreeError(f"{packs} is not a surface packs directory (it has no shared/)")
    return packs


def main() -> int:
    repo = Path(__file__).resolve().parent.parent
    try:
        print(tree(repo))
    except PinnedTreeError as e:
        print(f"pinned_surface_packs: {e}", file=sys.stderr)
        return 1
    return 0


if __name__ == "__main__":
    sys.exit(main())
