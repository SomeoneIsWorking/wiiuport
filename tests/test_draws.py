import pytest
from wiiuport.draws import DrawsWithoutUniforms

from wiiuport.control import ControlUnavailable

URL = "http://127.0.0.1:0/draws"


def _payload(prepared: int, rows: list[tuple[int, int, int]]) -> dict:
    return {
        "guestDrawsPrepared": prepared,
        "withoutVertexUniforms": [
            {"baseHash": base, "auxHash": aux, "draws": draws} for base, aux, draws in rows
        ],
    }


def test_a_stretch_of_play_counts_only_its_own_draws() -> None:
    start = DrawsWithoutUniforms.parse(URL, _payload(100, [(1, 0, 10)]))
    end = DrawsWithoutUniforms.parse(URL, _payload(300, [(1, 0, 10), (2, 5, 40)]))
    walked = end.since(start)
    assert walked.prepared == 200
    # A shader that drew nothing in the stretch is not listed.
    assert walked.byShader == {(2, 5): 40}
    rendered = walked.render()
    assert "the title drew 200 times, 40 of them with no vertex uniforms (20.0%)" in rendered
    assert "  0000000000000002/0000000000000005: 40 draws" in rendered


def test_a_stretch_where_no_draw_was_reported_is_refused() -> None:
    nothing = DrawsWithoutUniforms.parse(URL, _payload(0, []))
    with pytest.raises(ControlUnavailable, match="no draw was reported"):
        nothing.render()


def test_a_row_missing_its_count_is_refused() -> None:
    payload = _payload(1, [])
    payload["withoutVertexUniforms"] = [{"baseHash": 1, "auxHash": 0}]
    with pytest.raises(ControlUnavailable):
        DrawsWithoutUniforms.parse(URL, payload)
