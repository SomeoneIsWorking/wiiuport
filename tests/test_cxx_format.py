"""The formatting gate is trusted only because it has shown both answers.

It had shown neither: with no first-party C++ in the tree it examined zero
files on every run, so a broken invocation or a missing formatter would have
looked exactly like a pass.
"""

from __future__ import annotations

import shutil
from pathlib import Path

import pytest
from wiiuport.verify import check_formatting

FIXTURES = Path(__file__).parent / "fixtures" / "format"

pytestmark = pytest.mark.skipif(
    shutil.which("clang-format") is None,
    reason="clang-format is absent; its own refusal is covered separately",
)


def test_a_conforming_source_passes() -> None:
    passed, detail = check_formatting([FIXTURES / "formatted.cpp"], FIXTURES)
    assert passed, detail


def test_a_misformatted_source_is_reported() -> None:
    passed, detail = check_formatting([FIXTURES / "misformatted.cpp"], FIXTURES)
    assert not passed
    assert "misformatted.cpp" in detail


def test_the_configured_brace_rule_is_enforced() -> None:
    """InsertBraces is a claim about the configuration, so it gets its own
    discriminator: a source that is otherwise perfectly formatted and differs
    only by a missing brace must still be rejected."""
    passed, detail = check_formatting([FIXTURES / "unbraced.cpp"], FIXTURES)
    assert not passed, "an unbraced if body was accepted; InsertBraces is not in effect"
    assert "unbraced.cpp" in detail


def test_a_missing_formatter_refuses_instead_of_crashing(
    monkeypatch: pytest.MonkeyPatch,
) -> None:
    monkeypatch.setattr(shutil, "which", lambda _name: None)
    passed, detail = check_formatting([FIXTURES / "formatted.cpp"], FIXTURES)
    assert not passed
    assert "not on PATH" in detail and "dnf install" in detail
