"""The vertex census client must read every outcome the runtime counted, or refuse."""

from __future__ import annotations

import pytest

from wiiuport import vertex_census
from wiiuport.control import ControlUnavailable

URL = "http://127.0.0.1:0/vertices"


def shader(base_hash: int, **counts: int) -> dict:
    return {
        "baseHash": base_hash,
        "draws": {name: counts.get(name, 0) for name in vertex_census.OUTCOMES},
    }


def test_a_window_lists_the_most_stepped_shader_first() -> None:
    before = vertex_census.parse(URL, {"shaders": [shader(1, blended=5), shader(2, noPartner=1)]})
    after = vertex_census.parse(
        URL, {"shaders": [shader(1, blended=9, held=2), shader(2, noPartner=4), shader(3)]}
    )
    rows = vertex_census.window(before, after)
    assert [row.base_hash for row in rows] == [2, 1]
    assert rows[0].stepped == 3
    assert rows[1].draws["blended"] == 4 and rows[1].stepped == 0


def test_an_outcome_this_client_does_not_know_is_refused() -> None:
    row = shader(1)
    row["draws"]["teleported"] = 1
    with pytest.raises(ControlUnavailable, match="teleported"):
        vertex_census.parse(URL, {"shaders": [row]})


def test_totals_that_went_down_are_refused() -> None:
    before = vertex_census.parse(URL, {"shaders": [shader(1, blended=5)]})
    after = vertex_census.parse(URL, {"shaders": [shader(1, blended=2)]})
    with pytest.raises(ControlUnavailable, match="went down"):
        vertex_census.window(before, after)
