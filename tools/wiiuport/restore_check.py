"""Whether the title's frame comes back exactly after an in-between frame.

The runtime draws a frame of its own over the title's every tick and then
takes it back out. The check captures the title's frame before anything was
drawn over it and again after it was taken back out; those must be the same
image to the byte. Its control captures the in-between frame in place of the
second, which on a moving scene must differ: a comparison that finds even the
in-between frame identical is not comparing what it names.
"""

from __future__ import annotations

from dataclasses import dataclass

from wiiuport.control import DEFAULT_PORT, ControlUnavailable, read_counters, wait_for
from wiiuport.image import Image, arm_restore_check, bounding_box, compare, read_capture
from wiiuport.interpolation import read_interpolation

# Slots the runtime captures into: the title's frame, and what it is compared
# against.
GUEST_SLOT = 0
CHECK_SLOT = 1


class RestoreCheckRefused(Exception):
    """The runtime could not arm a capture, so a slot holds some other frame."""


@dataclass(frozen=True)
class Comparison:
    in_between: bool
    guest: Image
    other: Image
    bytes_total: int
    differing: int
    largest: int
    where: str

    def render(self) -> str:
        against = "in-between frame (control)" if self.in_between else "restored frame"
        return (
            f"title's frame against the {against}: {self.differing} of {self.bytes_total} "
            f"bytes differ (largest {self.largest}), at {self.where}"
        )


def comparison(guest: Image, other: Image, *, in_between: bool) -> Comparison:
    differing, largest, _ = compare(guest, other)
    return Comparison(
        in_between=in_between,
        guest=guest,
        other=other,
        bytes_total=len(guest.rgb),
        differing=differing,
        largest=largest,
        where=bounding_box(guest, other),
    )


def judge(restored: Comparison, control: Comparison) -> list[str]:
    """Why the pair does not show an exact restore; empty when it does."""
    problems = []
    if restored.in_between or not control.in_between:
        raise ValueError("judge takes the restored comparison first and the control second")
    if restored.differing != 0:
        problems.append(
            f"the title's frame did not come back: {restored.differing} bytes differ at "
            f"{restored.where}"
        )
    if control.differing == 0:
        problems.append(
            "the control found the in-between frame identical to the title's, so this "
            "comparison cannot tell a frame that came back from one that was never replaced"
        )
    return problems


def take(port: int = DEFAULT_PORT, *, in_between: bool, seconds: int = 30) -> Comparison:
    """Run one check at the next interpolated tick and compare its captures."""
    checks = read_interpolation(port)
    images = read_counters(port).imagesReceived
    arm_restore_check(port, in_between=in_between)

    def landed() -> None:
        seen = read_interpolation(port)
        if seen.restoreChecksRefused > checks.restoreChecksRefused:
            raise RestoreCheckRefused(
                "the runtime could not arm one of the check's captures, so a slot holds "
                "some other frame"
            )
        if seen.restoreChecksCompleted == checks.restoreChecksCompleted:
            raise ControlUnavailable("no interpolated tick has run the check yet")
        received = read_counters(port).imagesReceived
        if received < images + 2:
            raise ControlUnavailable(f"{received - images} of the check's 2 images have landed")

    wait_for(landed, seconds)
    guest = read_capture(port, slot=GUEST_SLOT)
    other = read_capture(port, slot=CHECK_SLOT)
    return comparison(guest, other, in_between=in_between)
