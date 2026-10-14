"""The ownership gate runs the shared tool for real and shows both answers."""

from __future__ import annotations

import json
import subprocess
from pathlib import Path

import pytest
from wiiuport.cxxpolicy import PolicyUnavailable, harness_tool, run_policy
from wiiuport.paths import Layout, find_layout

CLEAN = (
    "namespace project {\nclass Pad {\n  public:\n    int id() const;\n};\n} // namespace project\n"
)
STRAY = "int strayGlobal() {\n    static int counter = 0;\n    return ++counter;\n}\n"


def _project(root: Path, source: str) -> Path:
    (root / "src").mkdir(parents=True)
    (root / "src" / "unit.cpp").write_text(source)
    build = root / "build" / "lib"
    build.mkdir(parents=True)
    entry = {
        "directory": str(build),
        "file": str(root / "src" / "unit.cpp"),
        "arguments": ["clang++", "-std=c++20", "-c", str(root / "src" / "unit.cpp")],
    }
    (build / "compile_commands.json").write_text(json.dumps([entry]))
    real = find_layout().root
    for config in (".clang-tidy", ".clang-format"):
        (root / config).write_text((real / config).read_text())
    return build


def test_a_global_function_and_its_static_are_named(tmp_path: Path) -> None:
    build = _project(tmp_path, STRAY)
    report = run_policy(harness_tool(find_layout()), tmp_path, [build])
    assert report.units == 1
    assert not report.passed
    assert any("global-namespace function: strayGlobal" in line for line in report.findings)
    assert any("block-scope static: counter" in line for line in report.findings)


def test_a_class_in_a_namespace_passes(tmp_path: Path) -> None:
    build = _project(tmp_path, CLEAN)
    report = run_policy(harness_tool(find_layout()), tmp_path, [build])
    assert report.units == 1
    assert report.passed


def test_a_missing_database_refuses(tmp_path: Path) -> None:
    build = _project(tmp_path, CLEAN)
    with pytest.raises(PolicyUnavailable, match="build it first"):
        run_policy(harness_tool(find_layout()), tmp_path, [build, tmp_path / "absent"])


def test_no_checkout_refuses_naming_where_it_looked(tmp_path: Path) -> None:
    with pytest.raises(PolicyUnavailable, match="RE_HARNESS_DIR"):
        harness_tool(Layout(root=tmp_path / "a" / "b"), environ={})


def test_a_tool_that_fails_without_naming_a_site_refuses(tmp_path: Path) -> None:
    build = _project(tmp_path, CLEAN)

    def broken(command: list[str]) -> subprocess.CompletedProcess[str]:
        return subprocess.CompletedProcess(command, 2, "", "libclang not found")

    with pytest.raises(PolicyUnavailable, match="libclang not found"):
        run_policy(tmp_path / "cpp_policy.py", tmp_path, [build], run=broken)
