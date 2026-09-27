"""A capture run must hand back images that arrived after the arm.

A slot keeps whatever last landed in it. Polling a slot and taking the first
non-empty body therefore returns the *previous* run's image whenever the new one
has not arrived yet, and that is not a slow read -- it is a different answer. Two
rounds of a byte-for-byte comparison came back with identical checksums and a
paint count that had not moved, which is what a stale pair looks like and what a
re-capture does not.

So the wait is on the product's own count of images handed over, and these tests
are what make it trustworthy: the field is read rather than defaulted, a run that
does not get its images is a refusal rather than a short tuple, and a run that
never waits is the failure the watermark exists to catch.
"""

from __future__ import annotations

import sys
from pathlib import Path
from typing import Any

sys.path.insert(0, str(Path(__file__).resolve().parent.parent / "tools"))

import pytest

from wiiuport import control
from wiiuport.control import ControlUnavailable

FRAMED = b"0123456789abcdef"


class Recorder:
    """A product that answers with a fixed script, and remembers what it was asked.

    Stands in for the channel rather than the product: the thing under test is the
    *order* of the requests -- arm, wait for the watermark, read the slots -- and
    that order is only visible here.
    """

    def __init__(self, counts: list[Any], slots: dict[int, bytes]) -> None:
        self.counts = counts
        self.slots = slots
        self.calls: list[tuple[str, str]] = []
        # How many times the watermark has been asked, so the sequence advances on
        # the read rather than on how the fake happens to be written.
        self.setup_reads = 0

    def request_bytes(self, method: str, path: str, port: int, timeout: float) -> bytes:
        self.calls.append((method, path))
        if method == "POST" and path.startswith("/capture"):
            # The count is checked by the tests that call it, not here: what this
            # fake has to get right is the watermark it reports back.
            assert "count=" in path, f"the arm asked for no run: {path}"
            return f'{{"armed":true,"imagesReceived":{self.counts[0]}}}'.encode()
        if path == "/counters":
            return f'{{"imagesReceived":{self._next_count()}}}'.encode()
        if path.startswith("/capture?slot="):
            where = int(path.split("slot=")[1].split("&")[0])
            return self.slots.get(where, b"")
        raise AssertionError(f"unexpected request {method} {path}")

    def get(self, path: str, port: int, timeout: float) -> dict:
        self.calls.append(("GET", path))
        return {"imagesReceived": str(self._next_count())}

    def _next_count(self) -> int:
        index = min(self.setup_reads, len(self.counts)) - 1
        self.setup_reads += 1
        return self.counts[index]


def install(monkeypatch: pytest.MonkeyPatch, recorder: Recorder) -> None:
    monkeypatch.setattr(control, "request_bytes", recorder.request_bytes)
    monkeypatch.setattr(control, "_get", recorder.get)


def test_a_run_waits_for_its_own_images_before_reading_a_slot(
    monkeypatch: pytest.MonkeyPatch,
) -> None:
    """The watermark the arm reports is the floor, and the slots are read after it.

    Three counts, none of them at the watermark yet: the wait is the whole point,
    and a run that read a slot first would take the previous run's image.
    """
    recorder = Recorder([7, 8, 9, 9], {0: FRAMED, 1: FRAMED})
    install(monkeypatch, recorder)
    images = control.capture_run(21337, 2, timeout=5.0)
    assert images == (FRAMED, FRAMED)
    first_slot = next(index for index, (method, path) in enumerate(recorder.calls)
                      if method == "GET" and path.startswith("/capture?slot="))
    set_ups = [index for index, (_, path) in enumerate(recorder.calls) if path == "/counters"]
    assert set_ups, "the run never read the watermark"
    assert max(set_ups) < first_slot, (
        "a slot was read before the watermark said the images had arrived: "
        f"{recorder.calls}"
    )


def test_a_run_that_never_gets_its_images_is_a_refusal(monkeypatch: pytest.MonkeyPatch) -> None:
    """Short is a failure, not a shorter answer.

    A tuple of one image from a run of two would compare against a stale slot and
    report a difference that means nothing.
    """
    recorder = Recorder([7, 7, 7, 7, 7], {0: FRAMED, 1: b""})
    install(monkeypatch, recorder)
    with pytest.raises(ControlUnavailable) as refused:
        control.capture_run(21337, 2, timeout=0.4)
    assert "needed" in str(refused.value), str(refused.value)


def test_a_missing_watermark_is_not_read_as_zero(monkeypatch: pytest.MonkeyPatch) -> None:
    """None, not zero.

    A watermark that defaulted to zero would let the first image through as though
    it were new -- which is the failure the watermark is there to prevent, reached
    by making the field optional.
    """
    recorder = Recorder([0, 0], {0: FRAMED, 1: FRAMED})
    install(monkeypatch, recorder)
    monkeypatch.setattr(control, "_counter_field", lambda port, name, timeout=5.0: None)
    with pytest.raises(ControlUnavailable) as refused:
        control.capture_run(21337, 2, timeout=0.4)
    assert "only 0 images" in str(refused.value) or "needed" in str(refused.value), (
        str(refused.value)
    )


def test_a_run_of_nothing_is_refused_before_anything_is_asked(
    monkeypatch: pytest.MonkeyPatch,
) -> None:
    recorder = Recorder([0], {})
    install(monkeypatch, recorder)
    with pytest.raises(ControlUnavailable) as refused:
        control.capture_run(21337, 0, timeout=1.0)
    assert "not a run" in str(refused.value), str(refused.value)
    assert recorder.calls == [], f"it asked the product anyway: {recorder.calls}"
