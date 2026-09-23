"""The title's rate with interpolation on against off, in the same scenes.

A rate from one run compared with a rate from another compares two scenes as
much as two settings: the drive into the world does not land in the same
place twice, and the GPU cost of a frame differs several times over between
open sea and a town. So the two settings take turns in short windows of one
walk, and each is the sum of its own windows.
"""

from __future__ import annotations

from dataclasses import dataclass, field


@dataclass
class PairedRates:
    """Ticks and seconds the title spent with interpolation on, and off."""

    on_ticks: int = 0
    on_seconds: float = 0.0
    off_ticks: int = 0
    off_seconds: float = 0.0
    windows: list[tuple[bool, int, float]] = field(default_factory=list)

    def add(self, on: bool, ticks: int, seconds: float) -> None:
        if ticks < 0 or seconds <= 0.0:
            raise ValueError(f"a window of {ticks} ticks in {seconds} s is not a measurement")
        self.windows.append((on, ticks, seconds))
        if on:
            self.on_ticks += ticks
            self.on_seconds += seconds
        else:
            self.off_ticks += ticks
            self.off_seconds += seconds

    def rate(self, on: bool) -> float:
        ticks, seconds = (
            (self.on_ticks, self.on_seconds) if on else (self.off_ticks, self.off_seconds)
        )
        if seconds == 0.0:
            raise ValueError(f"no window was measured with interpolation {'on' if on else 'off'}")
        return ticks / seconds

    def render(self) -> str:
        on = self.rate(True)
        off = self.rate(False)
        return (
            f"paired windows: title rate {on:.2f} Hz with interpolation on, {off:.2f} Hz off "
            f"({100.0 * on / off:.1f}%), over {len(self.windows)} alternating windows"
        )
