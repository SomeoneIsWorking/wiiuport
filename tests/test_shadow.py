"""The shadow check's report: read as the runtime writes it, refused when it is not."""

from __future__ import annotations

import pytest
from wiiuport.shadow import ShadowReport

from wiiuport.control import ControlUnavailable

PAYLOAD = {
    "clean": True,
    "rounds": 40,
    "inBetweensDrawn": 40,
    "bytesCompared": 1000,
    "controlPages": 167,
    "inBetweenPages": 168,
    "charged": 0,
    "inBetweenOnlyCount": 1,
    "inBetweenOnly": [{"page": "105e0000", "windows": 1}],
    "refusal": "",
}


def test_a_report_names_each_listed_page_and_its_windows() -> None:
    report = ShadowReport.parse("/shadowcheck", PAYLOAD)
    assert report.inBetweenOnly == (("105e0000", 1),)
    assert "105e0000  in 1 of 40 in-between windows" in report.render()


def test_a_report_missing_a_field_is_refused() -> None:
    partial = {key: value for key, value in PAYLOAD.items() if key != "charged"}
    with pytest.raises(ControlUnavailable, match="charged"):
        ShadowReport.parse("/shadowcheck", partial)


def test_a_refused_check_says_it_measured_nothing() -> None:
    report = ShadowReport.parse("/shadowcheck", {**PAYLOAD, "clean": False, "refusal": "unmapped"})
    assert report.render() == "not measured: unmapped"
