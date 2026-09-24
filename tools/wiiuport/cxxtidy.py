"""clang-tidy over every first-party translation unit, with the flags it is built with.

Two builds compile first-party sources: the standalone library build (the
library, its tests and the maintainer tools) and the fork build, which alone
compiles the shell. Each unit is analysed from the database of the build that
compiles it, so no unit is linted with guessed flags and none is skipped
because the other database lacked it.
"""

from __future__ import annotations

import json
import os
import re
import subprocess
from collections.abc import Callable, Sequence
from dataclasses import dataclass
from pathlib import Path

from .hostdeps import MissingHostPackages, Requirement, check
from .paths import Layout
from .structure import FIRST_PARTY_CXX_ROOTS

TIDY_REQUIREMENT = Requirement(
    "clang-tidy", ("clang-tools-extra",), executables=("clang-tidy", "run-clang-tidy")
)

REQUIRED_CHECK_GROUPS: tuple[str, ...] = (
    "clang-analyzer-",
    "bugprone-",
    "performance-",
    "readability-braces-around-statements",
)
"""Groups `.clang-tidy` enables. Confirmed against `--list-checks`, because a
configuration that names a check the installed clang-tidy does not run is not
a gate."""

_DIAGNOSTIC = re.compile(
    r"^(?P<file>.+?):(?P<line>\d+):(?P<column>\d+): (?:warning|error): .+ \[(?P<check>[^\]]+)\]$"
)


class TidyUnavailable(RuntimeError):
    """clang-tidy could not be run at all, so nothing was analysed."""


@dataclass(frozen=True)
class Unit:
    """One translation unit and the build directory whose database compiles it."""

    source: Path
    database: Path


@dataclass(frozen=True)
class TidyReport:
    units: int
    diagnostics: tuple[str, ...]
    failed_runs: tuple[str, ...]

    @property
    def passed(self) -> bool:
        return not self.diagnostics and not self.failed_runs


Runner = Callable[[Sequence[str], Path], subprocess.CompletedProcess[str]]


def _run(command: Sequence[str], cwd: Path) -> subprocess.CompletedProcess[str]:
    return subprocess.run(list(command), cwd=cwd, capture_output=True, text=True, check=False)


def _is_first_party(source: Path, root: Path) -> bool:
    return any(source.is_relative_to(root / relative) for relative in FIRST_PARTY_CXX_ROOTS)


def tidy_units(root: Path, databases: Sequence[Path]) -> list[Unit]:
    """Every first-party ``.cpp`` any database compiles, from the first that does.

    Generated sources and precompiled-header stubs live under the build tree
    and are not first-party. A missing database refuses: the units only it
    compiles would otherwise go unanalysed and the gate would still pass.
    """
    root = root.resolve()
    units: dict[Path, Unit] = {}
    for database in databases:
        commands = database / "compile_commands.json"
        if not commands.is_file():
            raise TidyUnavailable(
                f"{commands} does not exist, so the units that build compiles were never "
                "analysed; build it first"
            )
        for entry in json.loads(commands.read_text()):
            source = (Path(entry["directory"]) / entry["file"]).resolve()
            if source.suffix == ".cpp" and _is_first_party(source, root):
                units.setdefault(source, Unit(source, database))
    return sorted(units.values(), key=lambda unit: unit.source)


def parse_diagnostics(output: str) -> list[str]:
    """Each distinct diagnostic line; a header's finding repeats once per unit including it."""
    found = {line for line in output.splitlines() if _DIAGNOSTIC.match(line)}
    return sorted(found)


def missing_check_groups(listed: str) -> list[str]:
    enabled = [line.strip() for line in listed.splitlines()]
    return [
        group for group in REQUIRED_CHECK_GROUPS if not any(c.startswith(group) for c in enabled)
    ]


def run_tidy(root: Path, units: Sequence[Unit], runner: Runner = _run) -> TidyReport:
    listed = runner(["clang-tidy", "--list-checks"], root)
    missing = missing_check_groups(listed.stdout)
    if listed.returncode != 0 or missing:
        raise TidyUnavailable(
            f"clang-tidy does not run the configured groups {missing}:\n"
            f"{(listed.stdout + listed.stderr).strip()}"
        )
    diagnostics: set[str] = set()
    failed: list[str] = []
    for database in dict.fromkeys(unit.database for unit in units):
        sources = [unit.source for unit in units if unit.database == database]
        pattern = "^(" + "|".join(re.escape(str(source)) for source in sources) + ")$"
        result = runner(
            [
                "run-clang-tidy",
                "-p",
                str(database),
                "-j",
                str(os.cpu_count() or 1),
                "-quiet",
                pattern,
            ],
            root,
        )
        output = result.stdout + result.stderr
        found = parse_diagnostics(output)
        diagnostics.update(found)
        # A failure with no diagnostic is a unit that did not parse: nothing in
        # it was checked, which is not a clean result.
        if result.returncode != 0 and not found:
            failed.append(
                f"clang-tidy failed over {database} without a diagnostic:\n{output.strip()}"
            )
    return TidyReport(len(units), tuple(sorted(diagnostics)), tuple(failed))


def check_tidy(layout: Layout) -> TidyReport:
    try:
        check((TIDY_REQUIREMENT,))
    except MissingHostPackages as missing:
        raise TidyUnavailable(f"clang-tidy was never run.\n{missing}") from missing
    units = tidy_units(layout.root, (layout.wiiuport_build, layout.cemu_build))
    if not units:
        raise TidyUnavailable("no database compiles a first-party unit, so nothing was analysed")
    return run_tidy(layout.root, units)
