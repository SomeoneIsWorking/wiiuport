import pytest
from wiiuport.draws import Draws, VertexCensus

from wiiuport.control import ControlUnavailable

URL = "http://127.0.0.1:0/draws"


def _rows(rows: list[tuple[int, int, int, int]]) -> list[dict]:
    return [
        {
            "baseHash": base,
            "auxHash": aux,
            "draws": draws,
            "compared": draws,
            "changed": changed,
            "bytesHashed": 64 * draws,
        }
        for base, aux, draws, changed in rows
    ]


def _payload(
    prepared: int,
    rows: list[tuple[int, int, int, int]],
    census: tuple[int, int, list[tuple[int, int, int, int]]] = (0, 0, []),
) -> dict:
    asked, taken, census_rows = census
    return {
        "guestDrawsPrepared": prepared,
        "withoutVertexUniforms": _rows(rows),
        "census": {
            "framesAsked": asked,
            "framesTaken": taken,
            "withVertexUniforms": _rows(census_rows),
        },
    }


def test_a_stretch_of_play_counts_only_its_own_draws() -> None:
    start = Draws.parse(URL, _payload(100, [(1, 0, 10, 0), (2, 5, 4, 1)]))
    end = Draws.parse(URL, _payload(300, [(1, 0, 10, 0), (2, 5, 44, 31)]))
    walked = end.since(start)
    assert walked.prepared == 200
    # A shader that drew nothing in the stretch is not listed.
    assert list(walked.withoutUniforms) == [(2, 5)]
    assert walked.withoutUniforms[(2, 5)].draws == 40
    assert walked.withoutUniforms[(2, 5)].changed == 30
    rendered = walked.render()
    assert "the title drew 200 times, 40 of them with no vertex uniforms (20.0%)" in rendered
    assert "30 of 40 compared read vertex bytes that changed" in rendered
    assert "(2560 bytes compared)" in rendered
    assert "  0000000000000002/0000000000000005: 40 draws, 30 of 40 compared changed" in rendered
    assert "  0000000000000002/0000000000000005: 30 of 40 compared draws changed" in rendered


def test_a_stretch_where_nothing_changed_says_so() -> None:
    still = Draws.parse(URL, _payload(10, [(1, 0, 5, 0)]))
    assert "most changed: (none)" in still.render()


def test_a_stretch_where_no_draw_was_reported_is_refused() -> None:
    nothing = Draws.parse(URL, _payload(0, []))
    with pytest.raises(ControlUnavailable, match="no draw was reported"):
        nothing.render()


def test_a_row_missing_its_count_is_refused() -> None:
    payload = _payload(1, [])
    payload["withoutVertexUniforms"] = [{"baseHash": 1, "auxHash": 0, "draws": 1}]
    with pytest.raises(ControlUnavailable):
        Draws.parse(URL, payload)


def test_a_finished_census_names_what_changed() -> None:
    census = Draws.parse(URL, _payload(10, [], (16, 16, [(3, 0, 200, 50), (4, 0, 100, 0)]))).census
    rendered = census.render()
    assert "over 16 frames, 50 of 300 compared draws that read uniforms (16.7%)" in rendered
    assert "  0000000000000003/0000000000000000: 50 of 200 compared draws changed" in rendered


def test_an_unfinished_census_is_refused() -> None:
    with pytest.raises(ControlUnavailable, match="took 3 of 16 frames"):
        VertexCensus(16, 3, {}).render()
    with pytest.raises(ControlUnavailable, match="took 0 of 0 frames"):
        VertexCensus(0, 0, {}).render()


def test_a_census_that_compared_nothing_is_refused() -> None:
    with pytest.raises(ControlUnavailable, match="compared no draw"):
        VertexCensus(2, 2, {}).render()
