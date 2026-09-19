"""Build and run the first-party C++ tests.

Separate from the gate so the verifier and a developer run the same code, and
so the count the gate reports is the count the harness actually executed rather
than a number the verifier made up.
"""

from __future__ import annotations

import re
import shutil
import subprocess
from dataclasses import dataclass
from pathlib import Path

from .paths import Layout

TEST_TARGET = "wiiuport_tests"

_SUMMARY = re.compile(r"(\d+) checks, (\d+) failures")


class CxxTestsUnavailable(RuntimeError):
    """The tests could not be built or run, so nothing was checked."""


@dataclass(frozen=True)
class CxxTestReport:
    checks: int
    failures: int
    output: str

    @property
    def passed(self) -> bool:
        return self.failures == 0 and self.checks > 0


def _run(command: list[str], cwd: Path) -> subprocess.CompletedProcess[str]:
    return subprocess.run(command, cwd=cwd, capture_output=True, text=True, check=False)


def build_and_run(layout: Layout) -> CxxTestReport:
    """Configure, build and run the harness, refusing by name at each step.

    Clang is the agent's verification compiler, so it is named here rather than
    left to whatever happens to be first on PATH. That is this command's own
    choice and not a constraint the project places on anyone building it.
    """
    for tool in ("cmake", "ninja", "clang++"):
        if shutil.which(tool) is None:
            raise CxxTestsUnavailable(
                f"{tool} is not on PATH, so the C++ tests were never built or run. "
                "Install it: sudo dnf install cmake ninja-build clang"
            )
    build = layout.wiiuport_build
    configure = _run(
        [
            "cmake",
            "-S",
            str(layout.root),
            "-B",
            str(build),
            "-G",
            "Ninja",
            "-DCMAKE_CXX_COMPILER=clang++",
            f"-DCMAKE_BUILD_TYPE={layout.build_type}",
            "-DWIIUPORT_BUILD_TESTS=ON",
        ],
        layout.root,
    )
    if configure.returncode != 0:
        raise CxxTestsUnavailable(
            "cmake could not configure the first-party build, so the C++ tests "
            f"were never run:\n{(configure.stdout + configure.stderr).strip()}"
        )
    _refuse_unless_clang(build)
    compiled = _run(["cmake", "--build", str(build), "--target", TEST_TARGET], layout.root)
    if compiled.returncode != 0:
        raise CxxTestsUnavailable(
            "the C++ tests did not compile, so none of them ran:\n"
            f"{(compiled.stdout + compiled.stderr).strip()}"
        )
    executed = _run([str(build / "tests" / "cxx" / TEST_TARGET)], layout.root)
    output = (executed.stdout + executed.stderr).strip()
    summary = _SUMMARY.search(output)
    if summary is None:
        raise CxxTestsUnavailable(
            "the test harness did not report how many checks it ran, so its exit "
            f"status cannot be read as a result:\n{output}"
        )
    return CxxTestReport(int(summary.group(1)), int(summary.group(2)), output)


def _refuse_unless_clang(build: Path) -> None:
    """Read the compiler out of the configured cache instead of assuming it.

    A tree already configured for another compiler keeps that compiler, so a
    green result here would be evidence about the wrong build.
    """
    settings = sorted(build.glob("CMakeFiles/*/CMakeCXXCompiler.cmake"))
    if not settings:
        raise CxxTestsUnavailable(
            f"{build} has no recorded C++ compiler, so the build it would produce "
            "cannot be attributed to any toolchain"
        )
    text = settings[0].read_text(encoding="utf-8")
    if 'CMAKE_CXX_COMPILER_ID "Clang"' not in text:
        identifier = re.search(r'CMAKE_CXX_COMPILER_ID "([^"]*)"', text)
        found = identifier.group(1) if identifier else "nothing recognisable"
        raise CxxTestsUnavailable(
            f"{build} is configured for {found}, not Clang. Remove that build tree "
            "and configure it again rather than accepting its result."
        )
