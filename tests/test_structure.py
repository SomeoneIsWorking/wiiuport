"""Structure limits must actually fire, and must exclude vendored upstream code."""

from __future__ import annotations

from pathlib import Path

from wiiuport.paths import Layout
from wiiuport.structure import DEFAULT_LINE_CAP, FIRST_PARTY_CXX_ROOTS, check_source_sizes


def _checkout(tmp_path: Path) -> Layout:
    (tmp_path / "docs").mkdir()
    (tmp_path / "docs" / "project-goals.md").write_text("x")
    (tmp_path / "src").mkdir()
    return Layout(root=tmp_path)


def test_an_oversized_file_is_reported_with_its_measured_size(tmp_path: Path) -> None:
    layout = _checkout(tmp_path)
    (layout.root / "src" / "huge.cpp").write_text("// line\n" * (DEFAULT_LINE_CAP + 7))
    findings = check_source_sizes(layout)
    assert len(findings) == 1
    assert "src/huge.cpp" in findings[0]
    assert str(DEFAULT_LINE_CAP + 7) in findings[0]
    assert str(DEFAULT_LINE_CAP) in findings[0]


def test_a_file_at_the_cap_passes(tmp_path: Path) -> None:
    layout = _checkout(tmp_path)
    (layout.root / "src" / "ok.cpp").write_text("// line\n" * DEFAULT_LINE_CAP)
    assert check_source_sizes(layout) == []


def test_vendored_upstream_is_not_subject_to_our_limits(tmp_path: Path) -> None:
    layout = _checkout(tmp_path)
    vendored = layout.root / "external" / "cemu" / "src"
    vendored.mkdir(parents=True)
    (vendored / "LatteCommandProcessor.cpp").write_text("// line\n" * 9000)
    assert check_source_sizes(layout) == []
    assert "external" not in FIRST_PARTY_CXX_ROOTS
