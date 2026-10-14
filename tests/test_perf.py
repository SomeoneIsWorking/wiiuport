"""The 60 Hz verdict `perf_run.py` prints, from a window's counts and percentiles."""

from __future__ import annotations

from wiiuport.perf import Percentiles, Window


def _present(p99_us: int, intervals: int = 3571) -> Percentiles:
    return Percentiles(
        intervals=intervals, p50_us=16_667, p95_us=19_460, p99_us=p99_us, longest_us=50_580
    )


def _window(paints: int = 3571, ticks: int = 1785, p99_us: int = 23_360, intervals: int = 3571):
    return Window(
        seconds=60.0, paints=paints, ticks=ticks, present=_present(p99_us, intervals), scan_out=None
    )


def test_a_measured_sustained_window_has_no_failures() -> None:
    assert _window().failures() == []


def test_parse_reads_the_channels_field_names() -> None:
    parsed = Percentiles.parse(
        {"intervals": 3571, "p50Us": 16667, "p95Us": 19460, "p99Us": 23360, "longestUs": 50580}
    )
    assert parsed == _present(23_360)


def test_each_shortfall_is_named() -> None:
    assert _window(paints=3489).failures() == ["painted 58.15/s, under 59.0"]
    assert _window(ticks=1745).failures() == ["logic ran 29.08/s, not 30"]
    assert _window(p99_us=33_090).failures() == ["p99 present interval 33.09 ms is over 25.00 ms"]
    assert "no present interval was measured" in _window(intervals=0).failures()


def test_a_p99_of_exactly_one_and_a_half_refreshes_passes() -> None:
    assert _window(p99_us=25_000).failures() == []
