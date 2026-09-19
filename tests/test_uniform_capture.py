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
    analyse,
    read_records,
)

_HEADER = struct.Struct("<7I2Q")


def _record(frame: int, values: list[float], *, stage: int = 0, base: int = 0xAA) -> bytes:
    payload = struct.pack(f"<{len(values)}f", *values)
    return (
        _HEADER.pack(RECORD_MAGIC, frame, stage, len(payload), 0, 4, -1 & 0xFFFFFFFF, base, 0xBB)
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
