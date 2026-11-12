#!/usr/bin/env python3
# SPDX-FileCopyrightText: 2026 fuddlesworth
# SPDX-License-Identifier: GPL-3.0-or-later
"""Validate bundled data JSON against the committed JSON Schemas.

The schemas live in two trees: phosphor-libs/data/schemas/ holds the shared
contracts and plasmazones/data/schemas/ the app-only ones. Some are the
single source of truth shared with runtime validation: phosphor-fsloader's
SchemaValidator (valijson) compiles the same schema file at load time. The
others are an author-time-only contract, because their document types
validate their content directly in C++ rather than through a runtime schema.
Which kind a schema is shows in whether a target embeds it as a Qt resource,
so it is not repeated here. Either way this script is the author-time gate —
run from lefthook on commit and from CI — so a malformed bundled data file
fails review rather than shipping and being skipped at runtime.

Every *.schema.json in a directory that holds a mapped schema must itself have
a SCHEMA_MAP entry. An unmapped schema governs nothing, so it fails the run.

A data file or schema is malformed, and fails, when it is not strict JSON. That
includes what json.load would otherwise let through: the bare tokens NaN,
Infinity and -Infinity, a number too large to be finite, and an object that
repeats a key. QJsonDocument refuses a file holding either kind of number at
run time, and a repeated key silently loses its earlier value there.

The schema dialect is Draft 7 (what valijson supports), validated here
with the `jsonschema` package's Draft7Validator so the two engines agree.

Usage:
    validate-json-schemas.py [--root DIR] [FILE ...]

Both CI and the lefthook hook run with no FILE arguments, so every data file
covered by the SCHEMA_MAP is validated (a schema edit re-checks all of its
data). Passing explicit FILEs is a manual convenience. A relative FILE is
resolved against --root, not against the current directory, because the
SCHEMA_MAP globs it is matched with are relative to the root too. A path that
matches no mapped glob is REPORTED by name and exits 1 rather than being
ignored, so a renamed or mistyped path cannot pass silently.

Exit status: 0 when everything validated, 1 on any validation failure, 2 when
the jsonschema validator could not be obtained or the command line is wrong.
"""

from __future__ import annotations

import argparse
import json
import math
import os
import posixpath
import sys
from pathlib import Path

# Sentinel set before re-execing into the bootstrapped interpreter, so a second
# failed import there aborts instead of looping.
_BOOTSTRAP_ENV = "PZ_JSONSCHEMA_BOOTSTRAPPED"

# The jsonschema release the bootstrapped venv installs. CI installs the same
# requirement (the `Install jsonschema` step of .github/workflows/ci.yml), so
# the local hook and CI judge the data with one validator. Change both together.
# A jsonschema the invoking interpreter can already import is used as found.
JSONSCHEMA_VERSION = "4.26.0"
JSONSCHEMA_REQUIREMENT = f"jsonschema[format]=={JSONSCHEMA_VERSION}"

# Maps a schema (relative to the source root) to the glob(s) of data
# files it governs. Add an entry here when a new document type gets a
# schema. Some schemas are also embedded via RCC and compiled by the
# runtime SchemaValidator; the others are validated only here, where the
# runtime checks those document types in C++ directly.
SCHEMA_MAP: dict[str, list[str]] = {
    "phosphor-libs/data/schemas/layout.schema.json": ["plasmazones/data/layouts/*.json"],
    "plasmazones/data/schemas/scrolling-template.schema.json": ["plasmazones/data/scrolling-templates/*.json"],
    "phosphor-libs/data/schemas/curve.schema.json": ["plasmazones/data/curves/*.json"],
    "phosphor-libs/data/schemas/animation-metadata.schema.json": ["plasmazones/data/animations/*/metadata.json"],
    "phosphor-libs/data/schemas/shader-metadata.schema.json": ["plasmazones/data/overlays/*/metadata.json"],
    "phosphor-libs/data/schemas/surface-metadata.schema.json": ["plasmazones/data/surface/*/metadata.json"],
    "phosphor-libs/data/schemas/pointer-metadata.schema.json": ["plasmazones/data/pointer/*/metadata.json"],
    "plasmazones/data/schemas/whatsnew.schema.json": ["plasmazones/data/whatsnew.json"],
    "phosphor-libs/data/schemas/host-descriptor.schema.json": ["plasmazones/data/placement-hosts/*.json"],
}

# The directories the mapped schemas live in, derived from SCHEMA_MAP so the two
# cannot drift. main() fails on a *.schema.json in one of them that has no entry
# above: nothing would be validated against it, and it would never be checked
# for being a well-formed draft-07 schema.
SCHEMA_DIRS: list[str] = sorted({posixpath.dirname(schema_rel) for schema_rel in SCHEMA_MAP})

# The one dialect this gate speaks. Draft7Validator is used unconditionally
# below, so every mapped schema has to declare the matching $schema or it
# would be validated under rules it did not ask for.
DRAFT7_URI = "http://json-schema.org/draft-07/schema#"


def fail(msg: str) -> None:
    print(f"validate-json-schemas: {msg}", file=sys.stderr)


def _refuse_constant(token: str) -> object:
    # json.load accepts the bare tokens NaN, Infinity and -Infinity, which are
    # not JSON. QJsonDocument refuses the whole file at run time, and here a
    # NaN would slip past every `minimum` and `maximum`.
    raise ValueError(f"{token} is not a JSON value")


def _finite_float(literal: str) -> float:
    # A literal such as 1e999 is well-formed JSON text that json.load turns into
    # infinity. QJsonDocument refuses it ("illegal number"), so refuse it here.
    value = float(literal)
    if not math.isfinite(value):
        raise ValueError(f"number {literal} is too large to be finite")
    return value


def _finite_int(literal: str) -> int:
    # An integer literal too long for a double is as illegal to QJsonDocument
    # as 1e999 is. float() of the literal's text overflows to infinity, where
    # float() of the parsed int would raise OverflowError instead.
    if not math.isfinite(float(literal)):
        raise ValueError(f"number {literal[:40]}... is too large to be finite")
    return int(literal)


def _refuse_duplicate_keys(pairs: list[tuple[str, object]]) -> dict[str, object]:
    # json.load keeps the last value of a repeated key without a word, and so
    # does QJsonDocument, so the earlier value is validated by nothing and read
    # by nothing while still sitting in the file for a reviewer to believe.
    obj: dict[str, object] = {}
    for key, value in pairs:
        if key in obj:
            raise ValueError(f"duplicate object key {key!r}")
        obj[key] = value
    return obj


def load_json(path: Path) -> object:
    """Parse PATH as strict JSON.

    Raises ValueError for anything that is not: json.JSONDecodeError and
    UnicodeDecodeError are both subclasses, and the hooks above raise it
    directly. Raises OSError when the file cannot be opened or read.
    """
    with path.open(encoding="utf-8") as handle:
        return json.load(
            handle,
            parse_constant=_refuse_constant,
            parse_float=_finite_float,
            parse_int=_finite_int,
            object_pairs_hook=_refuse_duplicate_keys,
        )


# Keywords whose values are literal data instances, not subschemas, so a
# "format" key nested inside them is a data field, not the JSON Schema format
# keyword, and must not be collected.
_INSTANCE_KEYWORDS = frozenset({"default", "const", "examples", "enum"})

# Keywords whose value is a map of subschemas keyed by arbitrary names. Those
# names can collide with _INSTANCE_KEYWORDS (e.g. a property literally named
# "default"), so recurse into the map's values directly rather than applying the
# instance-keyword skip to the arbitrary keys.
_SUBSCHEMA_MAP_KEYWORDS = frozenset({"properties", "patternProperties", "definitions", "$defs", "dependencies"})


def formats_used(node: object) -> set[str]:
    """Collect every JSON Schema `format` keyword string used in a schema.

    Distinguishes the `format` keyword from a data field named "format": values
    of instance keywords (default/const/examples/enum) are skipped, while
    subschema-map values (e.g. a property named "default") are still traversed.
    """
    found: set[str] = set()
    if isinstance(node, dict):
        fmt = node.get("format")
        if isinstance(fmt, str):
            found.add(fmt)
        for key, value in node.items():
            if key in _INSTANCE_KEYWORDS:
                continue
            if key in _SUBSCHEMA_MAP_KEYWORDS and isinstance(value, dict):
                for subschema in value.values():
                    found |= formats_used(subschema)
            else:
                found |= formats_used(value)
    elif isinstance(node, list):
        for item in node:
            found |= formats_used(item)
    return found


def venv_is_usable(venv_py: Path, require_pin: bool = True) -> bool:
    """Whether the cached venv's interpreter runs and holds jsonschema.

    With require_pin, it must hold the pinned release; without, any release.

    Run as a child process because that is the only honest test. venv.create()
    COPIES the interpreter binary on POSIX, so after a rolling-distro Python
    minor bump bin/python still exists and is still executable, and dies in the
    dynamic loader when the libpython it was linked against is gone. A file
    check sees nothing wrong with that; os.execv() into it replaces this process
    and the gate then exits 127 on every run, with nothing left to rebuild the
    venv. A venv built by an older copy of this script, holding a different
    jsonschema release, answers False here too and is rebuilt at the pin.
    """
    import subprocess

    probe = (
        "import sys, jsonschema; from importlib.metadata import version; "
        f"sys.exit(0 if version('jsonschema') == {JSONSCHEMA_VERSION!r} else 1)"
        if require_pin
        else "import jsonschema"
    )
    try:
        return subprocess.run(
            [str(venv_py), "-c", probe],
            stdout=subprocess.DEVNULL,
            stderr=subprocess.DEVNULL,
            check=False,
        ).returncode == 0
    except OSError:
        # Missing, dangling or not executable.
        return False


def ensure_jsonschema() -> None:
    """Make `jsonschema` importable, bootstrapping a cached venv if needed.

    Returns normally once jsonschema can be imported. If it is missing, this
    creates a cached venv under the user cache dir, installs
    JSONSCHEMA_REQUIREMENT into it, and re-execs this script with that
    interpreter. The venv is reused on later runs for as long as
    venv_is_usable() says its interpreter runs and holds the pinned release.
    Otherwise it is rebuilt, in a directory of its own beside it that replaces
    it only once the rebuild works. When a rebuild fails and the cached venv
    still validates with another release, that venv is used with a warning,
    and the rebuild is tried again on the next run.

    This keeps the lefthook hook a hard gate without forcing every contributor
    to install anything: a global `pip install` is unavailable or PEP 668
    blocked on most modern distros, so a managed venv is the portable path. CI
    already has jsonschema, so the import succeeds there and this is a no-op.

    Exits with status 2 only if the bootstrap itself fails (e.g. an offline
    first run, or a rebuild whose interpreter still cannot run) so the caller
    treats "validator unavailable" as a hard failure rather than silently
    skipping validation.
    """
    try:
        import jsonschema  # noqa: F401
        return
    except ImportError:
        pass

    if os.environ.get(_BOOTSTRAP_ENV) == "1":
        fail("jsonschema still unavailable after bootstrap")
        sys.exit(2)

    cache_root_env = os.environ.get("XDG_CACHE_HOME")
    try:
        cache_root = Path(cache_root_env) if cache_root_env else (Path.home() / ".cache")
    except RuntimeError:
        # Neither XDG_CACHE_HOME nor a resolvable home dir: nowhere to cache a
        # venv, so fail closed rather than crash with an uncaught traceback.
        fail("cannot locate a cache directory to bootstrap jsonschema (set XDG_CACHE_HOME or HOME)")
        sys.exit(2)
    venv_dir = cache_root / "plasmazones" / "jsonschema-venv"
    venv_py = venv_dir / "bin" / "python"
    # Success stamp written only AFTER pip install returns 0 and the new
    # interpreter has passed venv_is_usable(). Gate on BOTH the stamp and that
    # probe: the stamp catches a partially-built venv (e.g. an offline run where
    # venv creation succeeds but pip install fails), and the probe catches a
    # venv whose interpreter is missing or can no longer run. Either condition
    # rebuilds the venv rather than re-execing into a dead interpreter.
    ready_stamp = venv_dir / ".jsonschema-installed"

    if not (ready_stamp.exists() and venv_is_usable(venv_py)):
        print("validate-json-schemas: bootstrapping JSON-schema validator (one-time)...", file=sys.stderr)
        import shutil
        import subprocess
        import venv as venv_module

        import tempfile
        import time

        # The rebuild happens in a directory of this run's own beside the
        # venv, and replaces it only once it works: a rebuild that fails
        # (offline, say) never takes a working venv with it, and two runs
        # rebuilding at once never pull a tree out from under each other.
        # Leftovers of a run that was killed are swept once their directory
        # is an hour old. A build directory dates from its creation and a
        # venv renamed aside is touched when it is, so a rebuild in progress
        # is not swept unless it has run for an hour.
        cache_dir = venv_dir.parent
        build_dir: Path | None = None
        old_dir: Path | None = None
        try:
            cache_dir.mkdir(parents=True, exist_ok=True)
            for leftover in cache_dir.glob(venv_dir.name + ".*-*"):
                try:
                    if time.time() - leftover.stat().st_mtime > 3600:
                        shutil.rmtree(leftover, ignore_errors=True)
                except OSError:
                    pass
            build_dir = Path(tempfile.mkdtemp(prefix=venv_dir.name + ".build-", dir=cache_dir))
            build_py = build_dir / "bin" / "python"
            venv_module.create(build_dir, with_pip=True)
            subprocess.run(
                [str(build_py), "-m", "pip", "install", "--quiet", JSONSCHEMA_REQUIREMENT],
                check=True,
            )
            if not venv_is_usable(build_py):
                raise RuntimeError(f"the rebuilt interpreter {build_py} cannot run {JSONSCHEMA_REQUIREMENT}")
            # Stamped before the swap, so the venv that appears at venv_dir
            # carries its stamp from the start.
            (build_dir / ready_stamp.name).touch()
            # Rename the old venv aside and the new one into place: neither
            # name ever points at a half-deleted tree. A venv finds its
            # site-packages from the pyvenv.cfg beside its interpreter, so it
            # still works once moved. The venv is touched first where it can
            # be, so once it is aside it is not old enough for another run's
            # sweep. old_dir is set before the rename, so an interrupt between
            # the two leaves it tracked; the restore below tells a venv from
            # the empty directory by its pyvenv.cfg.
            if venv_dir.exists():
                try:
                    os.utime(venv_dir)
                except OSError:
                    pass
                old_dir = Path(tempfile.mkdtemp(prefix=venv_dir.name + ".old-", dir=cache_dir))
                try:
                    os.replace(venv_dir, old_dir)
                except FileNotFoundError:
                    # Another run moved it aside first. Its place is free.
                    shutil.rmtree(old_dir, ignore_errors=True)
                    old_dir = None
                except OSError:
                    shutil.rmtree(old_dir, ignore_errors=True)
                    old_dir = None
                    raise
            os.replace(build_dir, venv_dir)
            build_dir = None
            if not venv_is_usable(venv_py):
                raise RuntimeError(f"the moved interpreter {venv_py} cannot run {JSONSCHEMA_REQUIREMENT}")
        except BaseException as exc:  # noqa: BLE001 - restore what was there, whatever stopped the rebuild
            if build_dir is not None:
                shutil.rmtree(build_dir, ignore_errors=True)
            if old_dir is not None and not (old_dir / "pyvenv.cfg").exists():
                # Stopped before the old venv was moved there: nothing to
                # put back.
                shutil.rmtree(old_dir, ignore_errors=True)
                old_dir = None
            if old_dir is not None and venv_dir.exists() and venv_is_usable(venv_py):
                # A working venv at the pin is in place (this run's own, or
                # another run's), so the old one can go.
                shutil.rmtree(old_dir, ignore_errors=True)
                old_dir = None
            if old_dir is not None:
                # The swap went part or all of the way. Put the old venv back
                # in place of whatever is at venv_dir now: it validated before
                # this run, and the new one did not finish or does not run.
                try:
                    if venv_dir.exists():
                        failed = Path(tempfile.mkdtemp(prefix=venv_dir.name + ".old-", dir=cache_dir))
                        os.replace(venv_dir, failed)
                        shutil.rmtree(failed, ignore_errors=True)
                    os.replace(old_dir, venv_dir)
                    old_dir = None
                except OSError:
                    pass
            if not isinstance(exc, Exception):
                raise
            if ready_stamp.exists() and venv_is_usable(venv_py):
                # Another run finished the same rebuild first.
                pass
            elif ready_stamp.exists() and venv_is_usable(venv_py, require_pin=False):
                # The rebuild was only to move to the pinned release. The venv
                # that is there still validates, so use it rather than block
                # every commit until the network is back.
                fail(
                    f"could not rebuild jsonschema at {JSONSCHEMA_VERSION} ({exc}); "
                    "validating with the release already cached"
                )
            else:
                fail(f"could not bootstrap jsonschema ({exc}); install it manually or connect to a network")
                sys.exit(2)
        else:
            # The new venv is in place and runs, so the old one can go.
            if old_dir is not None:
                shutil.rmtree(old_dir, ignore_errors=True)

    # Re-exec this script under the bootstrapped interpreter, preserving args.
    os.environ[_BOOTSTRAP_ENV] = "1"
    try:
        os.execv(str(venv_py), [str(venv_py), os.path.abspath(__file__), *sys.argv[1:]])
    except OSError as exc:
        # Defense in depth: venv_is_usable() has just run this interpreter, so
        # this should be unreachable, but a dead interpreter must fail closed at
        # exit 2 rather than surface an uncaught traceback at exit 1.
        fail(f"could not run the bootstrapped jsonschema interpreter ({exc})")
        sys.exit(2)


def main() -> int:
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument(
        "--root",
        type=Path,
        default=Path(__file__).resolve().parent.parent,
        help="Source root that the SCHEMA_MAP paths are relative to "
             "(default: the repository containing this script). Deriving it\n"
             "from __file__ rather than the cwd keeps the script runnable\n"
             "from any directory; a cwd default made every schema look missing.",
    )
    parser.add_argument(
        "files",
        nargs="*",
        type=Path,
        help="Specific files to validate (default: every mapped data file). "
             "A relative path is resolved against --root, not the current directory.",
    )
    args = parser.parse_args()

    # Guarantee the validator is present (bootstraps a cached venv on first use
    # if needed, or exits non-zero if it cannot). Never silently skips.
    ensure_jsonschema()
    from jsonschema import Draft7Validator, FormatChecker

    # Enforce string `format` (e.g. "date") so this author-time gate matches
    # runtime: valijson checks date/date-time/time formats, and jsonschema
    # only does so when handed a FormatChecker. Keeps the two engines aligned.
    format_checker = FormatChecker()

    root: Path = args.root.resolve()

    # Resolve the explicit file list (if any) to absolute paths for
    # membership testing against each schema's matched set.
    requested: set[Path] | None = None
    if args.files:
        requested = {(p if p.is_absolute() else (root / p)).resolve() for p in args.files}

    # Which requested paths a glob actually matched, so one that matches
    # nothing can be reported instead of silently dropped.
    seen: set[Path] = set()
    failures = 0
    checked = 0

    for schema_dir in SCHEMA_DIRS:
        for unmapped in sorted((root / schema_dir).glob("*.schema.json")):
            unmapped_rel = unmapped.relative_to(root).as_posix()
            if unmapped_rel not in SCHEMA_MAP:
                fail(f"{unmapped_rel}: schema has no SCHEMA_MAP entry, so no data file is validated against it")
                failures += 1

    for schema_rel, globs in sorted(SCHEMA_MAP.items()):
        targets: list[Path] = []
        for pattern in globs:
            targets.extend(sorted(root.glob(pattern)))

        # Recorded here, ahead of every schema-level `continue` below. A
        # requested file whose schema is missing or invalid is then reported
        # once, by the schema's own failure, and not a second time as a path
        # that matched no mapped glob.
        if requested is not None:
            seen.update(resolved for resolved in (t.resolve() for t in targets) if resolved in requested)

        schema_path = root / schema_rel
        if not schema_path.is_file():
            fail(f"schema not found: {schema_rel}")
            failures += 1
            continue

        try:
            schema = load_json(schema_path)
            Draft7Validator.check_schema(schema)
        except Exception as exc:  # noqa: BLE001 - report any schema defect (parse or schema error)
            fail(f"invalid schema {schema_rel}: {exc}")
            failures += 1
            continue

        # check_schema() above validates against draft-07 whatever the schema
        # declares, so a schema that names a different draft would be checked
        # and then validated under rules it never asked for. Pin the dialect.
        # A bare `true` or `false` is a valid draft-07 schema and passes
        # check_schema(), but it has no "$schema" to declare the dialect with.
        declared = schema.get("$schema") if isinstance(schema, dict) else None
        if declared != DRAFT7_URI:
            fail(f"{schema_rel}: $schema must be {DRAFT7_URI} (found {declared!r})")
            failures += 1
            continue

        # Fail closed if the schema uses a `format` this environment can't
        # actually check (the jsonschema[format] extra supplies date-time, email,
        # uri, etc.; "date" is built in). Without this, an unenforceable format
        # would be silently skipped, letting a bad value pass locally while CI
        # (which installs the extra) rejects it. The bootstrap venv and CI both
        # have the extra, so this only guards a manual run on a bare jsonschema.
        unenforceable = sorted(f for f in formats_used(schema) if f not in format_checker.checkers)
        if unenforceable:
            fail(f"{schema_rel}: cannot enforce format(s) {', '.join(unenforceable)}; install jsonschema[format]")
            failures += 1
            continue

        validator = Draft7Validator(schema, format_checker=format_checker)

        # In full-set mode (no explicit files), a mapped glob that matches
        # nothing means a data directory was renamed/emptied out from under the
        # schema and validation silently stopped covering it. Fail rather than
        # report a misleading green. (Skipped in explicit-files mode, where a
        # schema legitimately has no staged files.)
        if requested is None and not targets:
            fail(f"{schema_rel}: no data files matched {', '.join(globs)} (mapping is stale?)")
            failures += 1
            continue

        for target in targets:
            if requested is not None and target.resolve() not in requested:
                continue
            checked += 1
            rel = target.relative_to(root)
            try:
                document = load_json(target)
            except ValueError as exc:
                fail(f"{rel}: malformed JSON: {exc}")
                failures += 1
                continue
            except OSError as exc:
                fail(f"{rel}: cannot read: {exc.strerror or exc}")
                failures += 1
                continue

            errors = sorted(validator.iter_errors(document), key=lambda e: list(e.path))
            if errors:
                failures += 1
                fail(f"{rel}: fails {schema_rel}")
                for err in errors:
                    location = "/" + "/".join(str(p) for p in err.path) if err.path else "(root)"
                    print(f"    {location}: {err.message}", file=sys.stderr)

    if requested is not None:
        unmatched = sorted(p for p in requested if p not in seen)
        if unmatched:
            for p in unmatched:
                fail(f"{p}: matched no mapped schema glob (renamed, mistyped, "
                     f"or outside the validated data trees)")
            failures += len(unmatched)

    if failures:
        fail(f"{failures} validation failure(s) across {checked} file(s)")
        return 1

    print(f"validate-json-schemas: OK ({checked} file(s) validated)")
    return 0


if __name__ == "__main__":
    sys.exit(main())
