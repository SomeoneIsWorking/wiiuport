"""The ownership checker is trusted only because it has shown both answers.

Each rule is asserted against a fixture that must be reported and a fixture
that must not be, so a checker that silently matched nothing would fail here.
"""

from __future__ import annotations

from pathlib import Path

import pytest
from wiiuport.cxxpolicy import CxxPolicyUnavailable, analyse

FIXTURES = Path(__file__).parent / "fixtures" / "cxx"


def _report(name: str):
    return analyse([FIXTURES / name], FIXTURES.resolve(), {})


def test_the_accepted_fixture_is_parsed_and_reports_nothing() -> None:
    report = _report("accepted.cpp")
    assert report.scanned == ("accepted.cpp",)
    assert report.findings == (), report.summary


def test_the_accepted_fixture_is_not_empty() -> None:
    """A fixture that declared nothing would pass the rule above for free."""
    body = (FIXTURES / "accepted.cpp").read_text(encoding="utf-8")
    assert "class Recorder" in body
    assert 'extern "C"' in body
    assert "const int* still_fine" in body


@pytest.mark.parametrize(
    ("rule", "name"),
    [
        ("global-namespace API", "orphan_function"),
        ("global-namespace API", "orphan_variable"),
        ("extern declaration", "borrowed_variable"),
        ("block-scope declaration", "cached"),
        ("block-scope declaration", "limit"),
        ("block-scope declaration", "stride"),
    ],
)
def test_every_rule_fires_on_the_rejected_fixture(rule: str, name: str) -> None:
    findings = _report("rejected.cpp").findings
    assert any(f.rule == rule and f"'{name}'" in f.detail for f in findings), [
        str(f) for f in findings
    ]


def test_each_finding_locates_itself() -> None:
    for finding in _report("rejected.cpp").findings:
        assert finding.file == "rejected.cpp"
        assert finding.line > 0


def test_an_unparsable_source_refuses_instead_of_reporting_clean(tmp_path: Path) -> None:
    broken = tmp_path / "broken.cpp"
    broken.write_text('#include "a_header_that_does_not_exist.h"\n', encoding="utf-8")
    with pytest.raises(CxxPolicyUnavailable) as refusal:
        analyse([broken], tmp_path, {})
    assert "broken.cpp" in str(refusal.value)


def test_the_report_states_its_denominator() -> None:
    assert "parsed 1 first-party translation units" in _report("accepted.cpp").summary
