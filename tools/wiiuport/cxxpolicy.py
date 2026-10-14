"""The shared C++ ownership gate: re-harness's `cpp_policy.py` over every first-party unit.

clang-tidy cannot forbid a project function in the global namespace, an `extern` of a project
symbol or a block-scope `static`; the shared tool reads those from Clang's AST, one compile
database at a time, and audits the project's `.clang-tidy` and `.clang-format`.
"""

from __future__ import annotations

import os
import re
import subprocess
import sys
from collections.abc import Callable, Mapping, Sequence
from dataclasses import dataclass
from pathlib import Path

from .paths import Layout

HARNESS_ENV = "RE_HARNESS_DIR"
TOOL = Path("tools") / "cpp_policy.py"
_SCANNED = re.compile(r"scanned (\d+) translation units")

Runner = Callable[[Sequence[str]], subprocess.CompletedProcess[str]]


class PolicyUnavailable(RuntimeError):
    """The shared tool or a database is missing, or the tool could not run."""


@dataclass(frozen=True)
class PolicyReport:
    units: int
    findings: tuple[str, ...]

    @property
    def passed(self) -> bool:
        return not self.findings


def harness_tool(layout: Layout, environ: Mapping[str, str] = os.environ) -> Path:
    """`cpp_policy.py` from `$RE_HARNESS_DIR`, else the first checkout where the layout looks."""
    candidates = (
        [Path(environ[HARNESS_ENV])]
        if HARNESS_ENV in environ
        else list(layout.re_harness_checkouts)
    )
    for checkout in candidates:
        tool = checkout / TOOL
        if tool.is_file():
            return tool
    tried = ", ".join(str(checkout) for checkout in candidates)
    raise PolicyUnavailable(
        f"no re-harness checkout holds {TOOL} (tried {tried}); clone "
        f"https://github.com/SomeoneIsWorking/re-harness there or set {HARNESS_ENV}"
    )


def _run(command: Sequence[str]) -> subprocess.CompletedProcess[str]:
    return subprocess.run(list(command), capture_output=True, text=True, check=False)


def run_policy(
    tool: Path, root: Path, databases: Sequence[Path], run: Runner = _run
) -> PolicyReport:
    audit = run([sys.executable, str(tool), "--audit-config", str(root)])
    findings = [line for line in audit.stdout.splitlines() if line.startswith(f"{root}: ")]
    if audit.returncode not in (0, 1) or (audit.returncode == 1 and not findings):
        raise PolicyUnavailable(f"the config audit could not run: {audit.stderr.strip()}")
    units = 0
    for database in databases:
        commands = database / "compile_commands.json"
        if not commands.is_file():
            raise PolicyUnavailable(f"{commands} does not exist; build it first")
        scan = run(
            [
                sys.executable,
                str(tool),
                "--compile-commands",
                str(commands),
                "--root",
                str(root),
                "--exclude",
                "external",
            ]
        )
        scanned = _SCANNED.search(scan.stdout)
        if scan.returncode not in (0, 1) or scanned is None:
            raise PolicyUnavailable(
                f"cpp_policy could not scan {commands}: {(scan.stderr or scan.stdout).strip()}"
            )
        sites = [line for line in scan.stdout.splitlines() if line.startswith(str(root))]
        if scan.returncode == 1 and not sites:
            raise PolicyUnavailable(f"cpp_policy failed over {commands} without naming a site")
        units += int(scanned.group(1))
        findings.extend(sites)
    return PolicyReport(units, tuple(dict.fromkeys(findings)))


def check_policy(layout: Layout) -> PolicyReport:
    return run_policy(harness_tool(layout), layout.root, (layout.wiiuport_build, layout.cemu_build))
