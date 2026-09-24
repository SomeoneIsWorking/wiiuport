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
import shlex
import subprocess
import sys
from collections.abc import Callable, Sequence
from dataclasses import dataclass
from pathlib import Path

from .lockedtools import LOCKED_BIN, LockedToolMissing, locked_tool
from .paths import Layout
from .structure import FIRST_PARTY_CXX_ROOTS

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


_PCH_LOAD = ("-Xclang", "-include-pch", "-Xclang")


def without_precompiled_header(arguments: Sequence[str]) -> list[str]:
    """A compile command that includes its precompiled header's source as text.

    A PCH is readable only by the exact compiler build that wrote it, and the
    pinned clang-tidy is never that build: it refused the host clang's PCH as
    'built from a different branch' with the same version number. CMake passes
    the load of the binary (``-Xclang -include-pch -Xclang <pch>``) and then
    the textual include of the header it was made from; dropping the first
    leaves a unit that sees exactly the same declarations.
    """
    kept: list[str] = []
    index = 0
    while index < len(arguments):
        if tuple(arguments[index : index + 3]) == _PCH_LOAD and index + 3 < len(arguments):
            index += 4
            continue
        kept.append(arguments[index])
        index += 1
    return kept


def tidy_database(database: Path, into: Path) -> Path:
    """A copy of ``database``'s compile commands with no precompiled header loaded."""
    entries = json.loads((database / "compile_commands.json").read_text())
    rewritten = []
    for entry in entries:
        arguments = entry.get("arguments") or shlex.split(entry["command"])
        rewritten.append(
            {
                "directory": entry["directory"],
                "file": entry["file"],
                "arguments": without_precompiled_header(arguments),
            }
        )
    into.mkdir(parents=True, exist_ok=True)
    (into / "compile_commands.json").write_text(json.dumps(rewritten))
    return into


def parse_diagnostics(output: str) -> list[str]:
    """Each distinct diagnostic line; a header's finding repeats once per unit including it."""
    found = {line for line in output.splitlines() if _DIAGNOSTIC.match(line)}
    return sorted(found)


def missing_check_groups(listed: str) -> list[str]:
    enabled = [line.strip() for line in listed.splitlines()]
    return [
        group for group in REQUIRED_CHECK_GROUPS if not any(c.startswith(group) for c in enabled)
    ]


def run_tidy(
    root: Path, units: Sequence[Unit], runner: Runner = _run, bin_dir: Path = LOCKED_BIN
) -> TidyReport:
    try:
        tidy = str(locked_tool("clang-tidy", bin_dir))
        parallel = str(locked_tool("run-clang-tidy.py", bin_dir))
    except LockedToolMissing as missing:
        raise TidyUnavailable(f"clang-tidy was never run.\n{missing}") from missing
    listed = runner([tidy, "--list-checks"], root)
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
                sys.executable,
                parallel,
                "-clang-tidy-binary",
                tidy,
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
    units = tidy_units(layout.root, (layout.wiiuport_build, layout.cemu_build))
    if not units:
        raise TidyUnavailable("no database compiles a first-party unit, so nothing was analysed")
    rewritten = {
        database: tidy_database(database, layout.build / "tidy" / database.name)
        for database in dict.fromkeys(unit.database for unit in units)
    }
    return run_tidy(layout.root, [Unit(unit.source, rewritten[unit.database]) for unit in units])
