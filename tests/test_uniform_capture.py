"""The slot discriminator is checked against both classes it must separate.

The synthetic records here are written by the test, so they prove the analysis
and the reader agree with each other -- not that either agrees with the
runtime. The format is defined in C++ by LatteUniformCapture, and the first
real capture is what confirms the two match; until then that is an untested
seam and is called out rather than assumed.
"""

from __future__ import annotations

import struct
from pathlib import Path

import pytest
from wiiuport.uniformcapture import (
    RECORD_MAGIC,
    CaptureUnreadable,
    ShaderAnalysis,
    SlotVerdict,
    analyse,
    clear_previous_capture,
    rank_for_review,
    read_records,
    without_frame_constant_slots,
)

_HEADER = struct.Struct("<8I2Q")
_SOURCE = struct.Struct("<2I")


def _record(
    frame: int,
    values: list[float],
    *,
    stage: int = 0,
    base: int = 0xAA,
    sources: tuple[tuple[int, int], ...] = (),
) -> bytes:
    payload = struct.pack(f"<{len(values)}f", *values)
    return (
        _HEADER.pack(
            RECORD_MAGIC,
            frame,
            stage,
            len(payload),
            0,
            4,
            -1 & 0xFFFFFFFF,
            len(sources),
            base,
            0xBB,
        )
        + b"".join(_SOURCE.pack(*source) for source in sources)
        + payload
    )


def _capture(tmp_path: Path, records: list[bytes]) -> Path:
    path = tmp_path / "uniform-capture.bin"
    path.write_bytes(b"".join(records))
    return path


def test_a_camera_slot_and_an_actor_slot_are_told_apart(tmp_path: Path) -> None:
    """Slot 0 is constant within each frame and moves between them; slot 1
    differs between draws in the same frame; slot 2 never changes at all."""
    records = [
        _record(0, [10.0, 1.0, 7.0]),
        _record(0, [10.0, 2.0, 7.0]),
        _record(0, [10.0, 3.0, 7.0]),
        _record(1, [20.0, 4.0, 7.0]),
        _record(1, [20.0, 5.0, 7.0]),
    ]
    analysis = analyse(list(read_records(_capture(tmp_path, records))))
    assert len(analysis) == 1
    verdicts = {s.offset: s.classification for s in analysis[0].slots}
    assert verdicts == {0: "frame-constant", 1: "per-draw", 2: "invariant"}


def test_an_unchanging_slot_is_not_called_a_camera(tmp_path: Path) -> None:
    """A constant is frame-constant in the literal sense and would be the
    easiest false positive here, so it gets its own class."""
    records = [_record(0, [5.0]), _record(0, [5.0]), _record(1, [5.0])]
    analysis = analyse(list(read_records(_capture(tmp_path, records))))
    assert analysis[0].slots[0].classification == "invariant"


def test_the_analysis_reports_its_denominators(tmp_path: Path) -> None:
    records = [_record(0, [1.0, 2.0]), _record(1, [3.0, 2.0])]
    summary = analyse(list(read_records(_capture(tmp_path, records))))[0].summary
    assert "2 draws over 2 frames" in summary
    assert "2 float slots" in summary


def test_shaders_are_analysed_separately(tmp_path: Path) -> None:
    records = [_record(0, [1.0], base=0xAA), _record(0, [2.0], base=0xCC)]
    analysis = analyse(list(read_records(_capture(tmp_path, records))))
    assert len({a.shader for a in analysis}) == 2


def test_a_missing_file_refuses_rather_than_reporting_no_slots(tmp_path: Path) -> None:
    with pytest.raises(CaptureUnreadable, match="does not exist"):
        list(read_records(tmp_path / "absent.bin"))


def test_an_empty_file_says_capture_ran_and_saw_nothing(tmp_path: Path) -> None:
    path = tmp_path / "uniform-capture.bin"
    path.write_bytes(b"")
    with pytest.raises(CaptureUnreadable, match="wrote nothing"):
        list(read_records(path))


def test_a_truncated_payload_is_refused(tmp_path: Path) -> None:
    good = _record(0, [1.0, 2.0, 3.0])
    path = _capture(tmp_path, [good[:-4]])
    with pytest.raises(CaptureUnreadable, match="truncated"):
        list(read_records(path))


def test_a_desynchronised_stream_is_refused(tmp_path: Path) -> None:
    path = _capture(tmp_path, [_record(0, [1.0]), b"\x00" * 40])
    with pytest.raises(CaptureUnreadable, match="desynchronised"):
        list(read_records(path))


def test_negative_layout_offsets_survive_the_unsigned_field(tmp_path: Path) -> None:
    """The runtime writes sint32 through a uint32 field, so an unused -1 slot
    must not come back as four billion."""
    record = next(read_records(_capture(tmp_path, [_record(0, [1.0])])))
    assert record.loc_remapped == -1
    assert record.count_uniform_register == 4


def _analysis(frame_constant: int, per_draw: int, draws: int) -> ShaderAnalysis:
    slots = [
        SlotVerdict(offset=i, draws=draws, distinct_within_frames=1, distinct_across_frames=2)
        for i in range(frame_constant)
    ]
    slots += [
        SlotVerdict(offset=100 + i, draws=draws, distinct_within_frames=3, distinct_across_frames=3)
        for i in range(per_draw)
    ]
    return ShaderAnalysis(shader=(0, draws, 0), draws=draws, frames=4, slots=tuple(slots))


def test_shaders_with_frame_constant_slots_rank_first() -> None:
    """The capture exists to find frame-constant slots, so a shader holding
    many of them outranks a busier shader holding none."""
    busy_but_dull = _analysis(frame_constant=0, per_draw=8, draws=9000)
    quiet_but_interesting = _analysis(frame_constant=3, per_draw=0, draws=4)
    ranked = rank_for_review([busy_but_dull, quiet_but_interesting])
    assert ranked[0] is quiet_but_interesting


def test_ties_are_broken_by_draw_count() -> None:
    fewer = _analysis(frame_constant=1, per_draw=0, draws=5)
    more = _analysis(frame_constant=1, per_draw=0, draws=500)
    assert rank_for_review([fewer, more])[0] is more


def test_the_dull_set_is_exactly_those_without_frame_constant_slots() -> None:
    dull = _analysis(frame_constant=0, per_draw=2, draws=7)
    keen = _analysis(frame_constant=2, per_draw=2, draws=7)
    assert without_frame_constant_slots([dull, keen]) == [dull]


def test_a_previous_capture_is_removed_before_a_run(tmp_path: Path) -> None:
    """Without this, a run that captures nothing copies the previous run's
    file out and reports it as its own result."""
    older = tmp_path / "uniform-capture.bin"
    older.write_bytes(b"stale")
    assert clear_previous_capture(older) == [older]
    assert not older.exists()


def test_clearing_a_capture_that_is_not_there_is_not_an_error(tmp_path: Path) -> None:
    assert clear_previous_capture(tmp_path / "absent.bin") == []


def test_uniform_block_sources_are_read_back(tmp_path: Path) -> None:
    """The address is what identifies an actor across ticks, so a record that
    carries one must not lose it on the way through the reader."""
    path = _capture(tmp_path, [_record(0, [1.0, 2.0], sources=((3, 0x1C4A0000), (4, 0x1C4B0000)))])
    record = next(iter(read_records(path)))
    assert record.sources == ((3, 0x1C4A0000), (4, 0x1C4B0000))
    assert record.source_addresses == (0x1C4A0000, 0x1C4B0000)


def test_a_draw_with_no_uniform_block_source_reads_as_empty(tmp_path: Path) -> None:
    record = next(iter(read_records(_capture(tmp_path, [_record(0, [1.0])]))))
    assert record.sources == ()


def test_an_impossible_source_count_is_refused(tmp_path: Path) -> None:
    """A desynchronised stream would otherwise be read as a record with
    millions of sources, allocating against garbage."""
    path = tmp_path / "uniform-capture.bin"
    path.write_bytes(_HEADER.pack(RECORD_MAGIC, 0, 0, 4, 0, 4, 0, 10_000, 0xAA, 0xBB) + b"\x00" * 4)
    with pytest.raises(CaptureUnreadable, match="uniform block sources"):
        list(read_records(path))


def test_a_capture_from_the_older_runtime_is_refused_by_magic(tmp_path: Path) -> None:
    """The layout changed. Reading an old file against the new header would
    silently shift every field rather than fail."""
    path = tmp_path / "uniform-capture.bin"
    old_header = struct.Struct("<7I2Q")
    path.write_bytes(old_header.pack(0x554E4946, 0, 0, 4, 0, 4, 0, 0xAA, 0xBB) + b"\x00" * 4)
    with pytest.raises(CaptureUnreadable, match="magic"):
        list(read_records(path))
