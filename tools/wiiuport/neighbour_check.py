"""Whether an in-between frame lies between the title's frames either side.

The runtime captures the title's frame of one interpolated tick, then the next
tick's in-between frame and its title's frame: A, M and C, three images the
display was shown in that order. M is judged against its neighbours by code,
from the pixels drawn rather than the values handed to the draws:

* A and C must differ, or the scene held still and nothing can be judged;
* M must differ from both, or it repeats a title frame rather than blending;
* M must be nearer each of them than they are to each other, by mean
  absolute difference: a frame between two others is, and one drawn
  elsewhere, or from the wrong frames, is not.
"""

from __future__ import annotations

from dataclasses import dataclass

from wiiuport.control import (
    DEFAULT_PORT,
    ControlUnavailable,
    read_counters,
    request_bytes,
    wait_for,
)
from wiiuport.image import Image, compare, read_capture
from wiiuport.interpolation import read_interpolation

# The runtime's slots for the three images, past the restore check's two.
BEFORE_SLOT = 2
IN_BETWEEN_SLOT = 3
AFTER_SLOT = 4


class NeighbourCheckRefused(Exception):
    """The runtime could not arm a capture, so a slot holds some other frame."""


@dataclass(frozen=True)
class Distance:
    """How far apart two of the images are."""

    differing: int
    mean: float


def distance(one: Image, other: Image) -> Distance:
    differing, _, mean = compare(one, other)
    return Distance(differing, mean)


@dataclass(frozen=True)
class Neighbours:
    before: Image
    between: Image
    after: Image
    before_to_after: Distance
    before_to_between: Distance
    between_to_after: Distance

    def render(self) -> str:
        return (
            "in-between frame against its neighbours, mean absolute difference: "
            f"title frames {self.before_to_after.mean:.3f} apart "
            f"({self.before_to_after.differing} bytes differ), "
            f"in-between {self.before_to_between.mean:.3f} from the one before "
            f"({self.before_to_between.differing}) and "
            f"{self.between_to_after.mean:.3f} from the one after "
            f"({self.between_to_after.differing})"
        )


def neighbours(before: Image, between: Image, after: Image) -> Neighbours:
    return Neighbours(
        before=before,
        between=between,
        after=after,
        before_to_after=distance(before, after),
        before_to_between=distance(before, between),
        between_to_after=distance(between, after),
    )


def judge(seen: Neighbours) -> list[str]:
    """Why the in-between frame is not shown to lie between its neighbours;
    empty when it is."""
    if seen.before_to_after.differing == 0:
        return [
            (
                "the title's two frames are identical, so the scene held still and an "
                "in-between frame cannot be judged against them"
            )
        ]
    problems = []
    if seen.before_to_between.differing == 0:
        problems.append("the in-between frame repeats the title's frame before it")
    if seen.between_to_after.differing == 0:
        problems.append("the in-between frame repeats the title's frame after it")
    span = seen.before_to_after.mean
    if seen.before_to_between.mean >= span:
        problems.append(
            f"the in-between frame is {seen.before_to_between.mean:.3f} from the frame before, "
            f"no nearer than the title's two frames are to each other ({span:.3f})"
        )
    if seen.between_to_after.mean >= span:
        problems.append(
            f"the in-between frame is {seen.between_to_after.mean:.3f} from the frame after, "
            f"no nearer than the title's two frames are to each other ({span:.3f})"
        )
    return problems


def take(port: int = DEFAULT_PORT, seconds: int = 30) -> Neighbours:
    """Run one check over the next two consecutive interpolated ticks."""
    checks = read_interpolation(port)
    images = read_counters(port).imagesReceived
    request_bytes("POST", "/neighbourcheck", port, 5.0)

    def landed() -> None:
        seen = read_interpolation(port)
        if seen.neighbourChecksRefused > checks.neighbourChecksRefused:
            raise NeighbourCheckRefused(
                "the runtime could not arm one of the check's captures, so a slot holds "
                "some other frame"
            )
        if seen.neighbourChecksCompleted == checks.neighbourChecksCompleted:
            raise ControlUnavailable("no two consecutive interpolated ticks have run the check yet")
        received = read_counters(port).imagesReceived
        # A restart captures the frame before again, so at least three land.
        if received < images + 3:
            raise ControlUnavailable(f"{received - images} of the check's 3 images have landed")

    wait_for(landed, seconds)
    return neighbours(
        read_capture(port, slot=BEFORE_SLOT),
        read_capture(port, slot=IN_BETWEEN_SLOT),
        read_capture(port, slot=AFTER_SLOT),
    )
