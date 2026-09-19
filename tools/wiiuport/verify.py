"""The project's normal verifier: the gates that must pass before work lands.

Every gate reports what it examined, including when it finds nothing wrong, so
a gate that silently examined an empty set cannot look like a gate that passed.
"""

from __future__ import annotations

import shutil
import subprocess
from dataclasses import dataclass
from pathlib import Path

from .cxxpolicy import CxxPolicyUnavailable, check_cxx_policy
from .paths import Layout
from .structure import FIRST_PARTY_CXX_ROOTS, check_source_sizes


@dataclass(frozen=True)
class GateResult:
    """One gate's outcome and what it looked at."""

    name: str
    passed: bool
    examined: int
    detail: str

    def render(self) -> str:
        status = "PASS" if self.passed else "FAIL"
        return f"[{status}] {self.name}: examined {self.examined}\n{self.detail}".rstrip()


def first_party_cxx_sources(layout: Layout) -> list[Path]:
    """First-party C++ only. Upstream Cemu under external/ is never included."""
    sources: list[Path] = []
    for relative in FIRST_PARTY_CXX_ROOTS:
        root = layout.root / relative
        if not root.is_dir():
            continue
        for suffix in ("*.cpp", "*.h", "*.hpp"):
            sources.extend(sorted(root.rglob(suffix)))
    return sources


def gate_python_lint(layout: Layout) -> GateResult:
    files = sorted((layout.root / "tools").rglob("*.py")) + sorted(
        (layout.root / "tests").rglob("*.py")
    )
    result = subprocess.run(
        ["uv", "run", "--frozen", "--group", "dev", "ruff", "check", "tools", "tests"],
        cwd=layout.root,
        capture_output=True,
        text=True,
        check=False,
    )
    return GateResult(
        "python lint (ruff)",
        result.returncode == 0,
        len(files),
        (result.stdout + result.stderr).strip(),
    )


def gate_python_format(layout: Layout) -> GateResult:
    """Non-mutating, so drift fails by file instead of being silently repaired
    by whoever runs the verifier next."""
    files = sorted((layout.root / "tools").rglob("*.py")) + sorted(
        (layout.root / "tests").rglob("*.py")
    )
    result = subprocess.run(
        ["uv", "run", "--frozen", "--group", "dev", "ruff", "format", "--check", "tools", "tests"],
        cwd=layout.root,
        capture_output=True,
        text=True,
        check=False,
    )
    return GateResult(
        "python format (ruff)",
        result.returncode == 0,
        len(files),
        (result.stdout + result.stderr).strip(),
    )


def gate_python_tests(layout: Layout) -> GateResult:
    result = subprocess.run(
        ["uv", "run", "--frozen", "--group", "dev", "python", "-m", "pytest", "-q"],
        cwd=layout.root,
        capture_output=True,
        text=True,
        check=False,
    )
    output = (result.stdout + result.stderr).strip()
    collected = output.count("passed") and output.splitlines()[-1] or output
    return GateResult(
        "python tests (pytest)",
        result.returncode == 0,
        len(sorted((layout.root / "tests").rglob("test_*.py"))),
        collected,
    )


def check_formatting(sources: list[Path], cwd: Path) -> tuple[bool, str]:
    """Run the non-mutating clang-format check over exactly these sources.

    Separate from the gate so tests drive the shipping implementation rather
    than a reimplementation of it. A missing formatter refuses by name: the
    bare subprocess call raised FileNotFoundError, which is a crash, not a
    check telling you what to install.
    """
    if shutil.which("clang-format") is None:
        return False, (
            "clang-format is not on PATH, so formatting was never checked. "
            "Install it: sudo dnf install clang-tools-extra"
        )
    result = subprocess.run(
        ["clang-format", "--dry-run", "--Werror", *[str(s) for s in sources]],
        cwd=cwd,
        capture_output=True,
        text=True,
        check=False,
    )
    return result.returncode == 0, (result.stdout + result.stderr).strip()


def gate_cxx_format(layout: Layout) -> GateResult:
    """Non-mutating clang-format check over first-party C++ only."""
    sources = first_party_cxx_sources(layout)
    if not sources:
        return GateResult(
            "c++ format (clang-format)",
            True,
            0,
            "no first-party C++ exists yet; upstream Cemu under external/ is "
            "vendored and is deliberately not reformatted",
        )
    passed, detail = check_formatting(sources, layout.root)
    return GateResult("c++ format (clang-format)", passed, len(sources), detail)


def gate_structure(layout: Layout) -> GateResult:
    findings = check_source_sizes(layout)
    return GateResult(
        "structure (source size limits)",
        not findings,
        len(first_party_cxx_sources(layout)) + len(sorted((layout.root / "tools").rglob("*.py"))),
        "\n".join(findings),
    )


def gate_cxx_policy(layout: Layout) -> GateResult:
    """The three ownership rules clang-tidy cannot express, on the real AST.

    A parse failure fails the gate rather than reporting an empty result: a
    file that could not be read had none of its declarations inspected.
    """
    try:
        report = check_cxx_policy(layout)
    except CxxPolicyUnavailable as unavailable:
        return GateResult("c++ ownership policy", False, 0, str(unavailable))
    detail = report.summary
    if report.findings:
        detail = "\n".join([detail, *(str(finding) for finding in report.findings)])
    return GateResult("c++ ownership policy", not report.findings, len(report.scanned), detail)


GATES = (
    gate_python_lint,
    gate_python_format,
    gate_python_tests,
    gate_cxx_format,
    gate_cxx_policy,
    gate_structure,
)


def run_all(layout: Layout) -> list[GateResult]:
    return [gate(layout) for gate in GATES]
