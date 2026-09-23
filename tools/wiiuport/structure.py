"""Mechanical structure limits, enforced by the normal verifier.

A limit is never raised to land a change. Legacy entries only ratchet down.
"""

from __future__ import annotations

from pathlib import Path

from .paths import Layout

FIRST_PARTY_CXX_ROOTS: tuple[str, ...] = ("src", "tests/cxx", "tools/cxx")
"""Upstream Cemu lives under external/ and is out of scope for every gate here."""

DEFAULT_LINE_CAP = 1200
"""Default source-file cap. 2000+ lines is critical extraction territory."""

CRITICAL_LINE_CAP = 2000

LEGACY_LIMITS: dict[str, int] = {}
"""Known oversized first-party files, each pinned at its current size so it
cannot grow. Entries are removed as code is extracted, never raised."""


def _tracked_sources(layout: Layout) -> list[Path]:
    sources: list[Path] = []
    for relative in FIRST_PARTY_CXX_ROOTS:
        root = layout.root / relative
        if root.is_dir():
            for suffix in ("*.cpp", "*.h", "*.hpp"):
                sources.extend(sorted(root.rglob(suffix)))
    tools = layout.root / "tools"
    if tools.is_dir():
        sources.extend(sorted(tools.rglob("*.py")))
    return sources


def check_source_sizes(layout: Layout) -> list[str]:
    """Return one finding per oversized file, naming the file and its measured size."""
    findings: list[str] = []
    for source in _tracked_sources(layout):
        relative = source.relative_to(layout.root).as_posix()
        lines = len(source.read_text(encoding="utf-8", errors="replace").splitlines())
        cap = LEGACY_LIMITS.get(relative, DEFAULT_LINE_CAP)
        if lines > cap:
            severity = "CRITICAL " if lines >= CRITICAL_LINE_CAP else ""
            findings.append(
                f"{severity}{relative}: {lines} lines exceeds the {cap}-line limit; "
                "split by responsibility rather than raising the limit"
            )
    return findings
