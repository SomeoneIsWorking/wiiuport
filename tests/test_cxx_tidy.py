"""The clang-tidy gate is trusted only once it has shown both answers."""

from __future__ import annotations

import json
import subprocess
from collections.abc import Sequence
from pathlib import Path

import pytest
from wiiuport.cxxtidy import (
    TidyUnavailable,
    Unit,
    missing_check_groups,
    parse_diagnostics,
    run_tidy,
    tidy_units,
)

FINDING = (
    "/r/src/wiiuport/a.cpp:3:7: error: 2 adjacent parameters of 'f' of similar type "
    "are easily swapped by mistake [bugprone-easily-swappable-parameters,-warnings-as-errors]"
)
ALL_GROUPS = "Enabled checks:\n    bugprone-sizeof-expression\n    clang-analyzer-core.NullDereference\n    performance-enum-size\n    readability-braces-around-statements\n"


def _database(directory: Path, files: Sequence[str]) -> Path:
    directory.mkdir(parents=True)
    entries = [
        {"directory": str(directory), "file": name, "command": "clang++ -c"} for name in files
    ]
    (directory / "compile_commands.json").write_text(json.dumps(entries))
    return directory


def test_only_first_party_units_are_taken_each_from_the_first_database(tmp_path: Path) -> None:
    root = tmp_path
    library = _database(
        root / "build" / "wiiuport",
        [
            str(root / "src/wiiuport/a.cpp"),
            str(root / "tests/cxx/t.cpp"),
            str(root / "build/gen.cpp"),
        ],
    )
    fork = _database(
        root / "build" / "cemu",
        [
            str(root / "src/wiiuport/a.cpp"),
            str(root / "src/wiiuport/shell/Main.cpp"),
            str(root / "external/cemu/src/x.cpp"),
        ],
    )
    units = tidy_units(root, (library, fork))
    assert [(u.source.relative_to(root.resolve()).as_posix(), u.database.name) for u in units] == [
        ("src/wiiuport/a.cpp", "wiiuport"),
        ("src/wiiuport/shell/Main.cpp", "cemu"),
        ("tests/cxx/t.cpp", "wiiuport"),
    ]


def test_a_missing_database_refuses_rather_than_skipping_its_units(tmp_path: Path) -> None:
    library = _database(tmp_path / "build" / "wiiuport", [])
    with pytest.raises(TidyUnavailable, match="compile_commands.json does not exist"):
        tidy_units(tmp_path, (library, tmp_path / "build" / "cemu"))


def test_a_diagnostic_is_parsed_once_however_often_it_repeats() -> None:
    output = f"{FINDING}\n  f(a, b);\n{FINDING}\n2 warnings generated.\n"
    assert parse_diagnostics(output) == [FINDING]
    assert parse_diagnostics("clang-tidy ran over 3 files\n") == []


def test_an_unrun_check_group_is_named() -> None:
    assert missing_check_groups(ALL_GROUPS) == []
    assert missing_check_groups(ALL_GROUPS.replace("performance-enum-size", "")) == ["performance-"]


def _runner(tidy: subprocess.CompletedProcess[str]):
    def run(command: Sequence[str], cwd: Path) -> subprocess.CompletedProcess[str]:
        if command[0] == "clang-tidy":
            return subprocess.CompletedProcess(command, 0, ALL_GROUPS, "")
        return tidy

    return run


UNITS = [Unit(Path("/r/src/wiiuport/a.cpp"), Path("/r/build/cemu"))]


def test_a_finding_fails_the_gate() -> None:
    report = run_tidy(Path("/r"), UNITS, _runner(subprocess.CompletedProcess([], 1, FINDING, "")))
    assert not report.passed
    assert report.diagnostics == (FINDING,)


def test_a_clean_run_passes() -> None:
    report = run_tidy(Path("/r"), UNITS, _runner(subprocess.CompletedProcess([], 0, "", "")))
    assert report.passed
    assert report.units == 1


def test_a_failed_run_without_a_diagnostic_is_not_clean() -> None:
    broken = subprocess.CompletedProcess([], 1, "", "Error while processing a.cpp")
    report = run_tidy(Path("/r"), UNITS, _runner(broken))
    assert not report.passed
    assert "without a diagnostic" in report.failed_runs[0]
