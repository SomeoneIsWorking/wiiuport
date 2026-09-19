"""The C++ test gate's own refusals.

The gate turns a harness run into a pass or a fail, so its reading of that run
is the part that can lie: a harness that built, exited zero and asserted
nothing must not become a green line. These drive the shipping report type and
the shipping summary pattern rather than restating them.
"""

from __future__ import annotations

import pytest
from wiiuport.cxxtests import _SUMMARY, CxxTestReport


def test_zero_checks_is_not_a_pass() -> None:
    report = CxxTestReport(checks=0, failures=0, output="0 checks, 0 failures")
    assert not report.passed


def test_failures_are_not_a_pass() -> None:
    assert not CxxTestReport(checks=26, failures=1, output="").passed


def test_a_clean_run_passes() -> None:
    assert CxxTestReport(checks=26, failures=0, output="").passed


@pytest.mark.parametrize(
    ("output", "expected"),
    [
        ("26 checks, 0 failures", (26, 0)),
        ("FAIL something\n26 checks, 1 failures", (26, 1)),
    ],
)
def test_the_summary_is_read_from_the_harness_output(
    output: str, expected: tuple[int, int]
) -> None:
    found = _SUMMARY.search(output)
    assert found is not None
    assert (int(found.group(1)), int(found.group(2))) == expected


def test_output_without_a_summary_is_not_read_as_a_result() -> None:
    """A harness that crashed before printing must refuse, not be scored zero."""
    assert _SUMMARY.search("Segmentation fault") is None
