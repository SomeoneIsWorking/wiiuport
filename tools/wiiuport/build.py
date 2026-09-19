"""Configure and build the pinned Cemu fork.

Build policy lives here, not in a shell script and not duplicated in CI YAML.
CI and maintainers call this module so there is one definition of how the
runtime is built.
"""

from __future__ import annotations

import os
import re
import subprocess
from dataclasses import dataclass
from pathlib import Path

from .paths import Layout

FAILURE_EXCERPT_LINES = 40
"""How much of a failed command's log to inline in the refusal."""

GENERATOR = "Ninja"
"""Ninja, not Unix Makefiles: Cemu's corpus is large enough that a Makefile
generator rebuilds every object after a reconfigure, while Ninja compares the
actual compiler commands and keeps valid objects."""


class BuildError(RuntimeError):
    """A configure or compile step failed, or produced a tree we will not trust."""


@dataclass(frozen=True)
class Toolchain:
    """The C and C++ compilers a build is configured with."""

    c_compiler: str = "clang"
    cxx_compiler: str = "clang++"

    @property
    def expected_cmake_id(self) -> str:
        return "Clang"


@dataclass(frozen=True)
class BuildConfig:
    """One build tree's full configuration."""

    layout: Layout
    build_type: str = "RelWithDebInfo"
    toolchain: Toolchain = Toolchain()

    @property
    def build_dir(self) -> Path:
        return self.layout.cemu_build

    @property
    def binary(self) -> Path:
        return Layout(root=self.layout.root, build_type=self.build_type).cemu_binary


def _cmake_cache_value(build_dir: Path, key: str) -> str | None:
    cache = build_dir / "CMakeCache.txt"
    if not cache.is_file():
        return None
    pattern = re.compile(rf"^{re.escape(key)}(?::[^=]*)?=(.*)$", re.MULTILINE)
    match = pattern.search(cache.read_text(encoding="utf-8", errors="replace"))
    return match.group(1).strip() if match else None


def _vcpkg_environment() -> dict[str, str]:
    env = dict(os.environ)
    env["VCPKG_FORCE_SYSTEM_BINARIES"] = "1"
    env.setdefault("VCPKG_MAX_CONCURRENCY", str(os.cpu_count() or 1))
    return env


def configure(config: BuildConfig, log: Path | None = None) -> None:
    """Configure the build tree, refusing a tree left by another generator.

    A cache configured for another generator or compiler is not silently
    reused: the caller is told which build directory to clean, because
    reconfiguring in place is what produces a tree whose evidence cannot be
    trusted.
    """
    source = config.layout.require_cemu_source()
    build_dir = config.build_dir
    existing_generator = _cmake_cache_value(build_dir, "CMAKE_GENERATOR")
    if existing_generator is not None and existing_generator != GENERATOR:
        raise BuildError(
            f"{build_dir} is configured with generator {existing_generator!r}, "
            f"but this project builds with {GENERATOR!r}. Remove that exact "
            f"directory and configure again: rm -rf {build_dir}"
        )
    build_dir.mkdir(parents=True, exist_ok=True)
    command = [
        "cmake",
        "-S", str(source),
        "-B", str(build_dir),
        "-G", GENERATOR,
        f"-DCMAKE_BUILD_TYPE={config.build_type}",
        f"-DCMAKE_C_COMPILER={config.toolchain.c_compiler}",
        f"-DCMAKE_CXX_COMPILER={config.toolchain.cxx_compiler}",
        "-DCMAKE_EXPORT_COMPILE_COMMANDS=ON",
    ]
    _run(command, log=log, what="configure")
    verify_toolchain(config)


def verify_toolchain(config: BuildConfig) -> None:
    """Read back the compiler the tree was actually configured with.

    The policy is that agent evidence builds use Clang. Asserting that from the
    command line is not enough; a stale cache can silently disagree.
    """
    actual = _cmake_cache_value(config.build_dir, "CMAKE_CXX_COMPILER_ID")
    if actual is None:
        raise BuildError(
            f"{config.build_dir} has no CMAKE_CXX_COMPILER_ID; the tree is not "
            "configured, so no build from it can be trusted as evidence"
        )
    expected = config.toolchain.expected_cmake_id
    if actual != expected:
        raise BuildError(
            f"{config.build_dir} is configured with CMAKE_CXX_COMPILER_ID="
            f"{actual!r}, expected {expected!r}"
        )


def compile_all(config: BuildConfig, log: Path | None = None) -> Path:
    """Build the tree and return the produced binary, refusing a missing one."""
    verify_toolchain(config)
    _run(["cmake", "--build", str(config.build_dir)], log=log, what="build")
    binary = config.binary
    if not binary.is_file():
        produced = sorted(p.name for p in binary.parent.glob("Cemu_*") if p.is_file())
        raise BuildError(
            f"the build reported success but {binary} does not exist. "
            f"bin/ holds: {produced or '(nothing)'}"
        )
    return binary


def _run(command: list[str], *, log: Path | None, what: str) -> None:
    if log is None:
        result = subprocess.run(command, env=_vcpkg_environment(), check=False)
        output_hint = ""
    else:
        log.parent.mkdir(parents=True, exist_ok=True)
        with log.open("w", encoding="utf-8") as handle:
            result = subprocess.run(
                command, env=_vcpkg_environment(), check=False,
                stdout=handle, stderr=subprocess.STDOUT,
            )
        output_hint = f" Full output: {log}"
    if result.returncode != 0:
        raise BuildError(
            f"{what} failed with exit code {result.returncode}: "
            f"{' '.join(command)}.{output_hint}{_failure_excerpt(log)}"
        )


def _failure_excerpt(log: Path | None) -> str:
    """The tail of the log, inline in the refusal.

    A path alone is useless wherever the tree does not outlive the run, which
    is every hosted job: CI reported only that configure exited 1 and pointed
    at a file the runner had already discarded.
    """
    if log is None or not log.is_file():
        return ""
    lines = log.read_text(encoding="utf-8", errors="replace").splitlines()
    tail = lines[-FAILURE_EXCERPT_LINES:]
    shown = f"last {len(tail)} of {len(lines)} lines"
    return "\n--- " + shown + " ---\n" + "\n".join(tail)
