"""What a performance window measured, and whether it is 60 Hz sustained.

Sustained means every picture the title paints reaches a refresh: the paint rate holds 60 while
the logic holds its own 30, and the 99th percentile present interval stays under one and a half
refreshes, so at most one present in a hundred waited an extra refresh.
"""

from __future__ import annotations

from dataclasses import dataclass

REFRESH_US = 1_000_000 / 60
P99_LIMIT_US = 1.5 * REFRESH_US
PAINT_RATE_MIN = 59.0
LOGIC_RATE = (29.5, 30.5)


@dataclass(frozen=True)
class Percentiles:
    intervals: int
    p50_us: int
    p95_us: int
    p99_us: int
    longest_us: int

    @staticmethod
    def parse(payload: dict) -> Percentiles:
        return Percentiles(
            intervals=int(payload["intervals"]),
            p50_us=int(payload["p50Us"]),
            p95_us=int(payload["p95Us"]),
            p99_us=int(payload["p99Us"]),
            longest_us=int(payload["longestUs"]),
        )

    def render(self) -> str:
        return (
            f"{self.intervals} intervals, p50 {self.p50_us / 1000:.2f} ms, "
            f"p95 {self.p95_us / 1000:.2f} ms, p99 {self.p99_us / 1000:.2f} ms, "
            f"longest {self.longest_us / 1000:.2f} ms"
        )


@dataclass(frozen=True)
class Window:
    seconds: float
    paints: int
    ticks: int
    present: Percentiles
    scan_out: Percentiles | None

    @property
    def paint_rate(self) -> float:
        return self.paints / self.seconds

    @property
    def logic_rate(self) -> float:
        return self.ticks / self.seconds

    def failures(self) -> list[str]:
        """Each way the window falls short of 60 Hz sustained; empty when it does not."""
        found = []
        if self.present.intervals == 0:
            found.append("no present interval was measured")
        if self.paint_rate < PAINT_RATE_MIN:
            found.append(f"painted {self.paint_rate:.2f}/s, under {PAINT_RATE_MIN}")
        if not LOGIC_RATE[0] <= self.logic_rate <= LOGIC_RATE[1]:
            found.append(f"logic ran {self.logic_rate:.2f}/s, not 30")
        if self.present.p99_us > P99_LIMIT_US:
            found.append(
                f"p99 present interval {self.present.p99_us / 1000:.2f} ms is over "
                f"{P99_LIMIT_US / 1000:.2f} ms"
            )
        return found

    def render(self) -> str:
        lines = [
            (
                f"window: {self.seconds:.1f} s, {self.paint_rate:.2f} paints/s, "
                f"logic {self.logic_rate:.2f} ticks/s"
            ),
            f"present: {self.present.render()}",
        ]
        if self.scan_out is not None:
            lines.append(f"scan-out: {self.scan_out.render()}")
        return "\n".join(lines)
