#!/usr/bin/env python3
"""Which bytes of a uniform block are the pose, read from two of its dumps.

The title's per-object uniform block is the place its pose lives -- the node's
own draw binds it and the binder computes its address -- so finding the pose
means finding which bytes of it change when a tick goes by, and then saying
*what* those bytes are rather than only that they moved.

So: two dumps of the same block, a tick apart, and this reports every aligned
four-byte group that differs as the two floats it could be, then looks for the
shape a rigid transform has -- three consecutive three-vectors of length one,
mutually perpendicular, with a translation beside them. A group that moves and
has that shape is a transform; a group that moves and does not is a float, a
colour, or a count, and is reported as such.

Nothing here decides where the pose is. It reports what is at each offset, and a
reader with the title's own naming in hand -- `cWorldViewMatrix` is in its
rodata -- says which is which. What it refuses to do is pick the largest changing
range and call it the camera, which is the mistake the mechanism being retired
made.
"""

from __future__ import annotations

import argparse
import math
import struct
import sys
from pathlib import Path

# A vector of three is a unit vector if its length is within this of one. Loose
# enough for a float32 read twice, tight enough that a random float is not one.
UNIT_TOLERANCE = 0.01


def floats(data: bytes) -> list[float]:
    return list(struct.unpack(f"!{len(data) // 4}f", data[: (len(data) // 4) * 4]))


def length3(v: tuple[float, float, float]) -> float:
    return math.sqrt(v[0] * v[0] + v[1] * v[1] + v[2] * v[2])


def dot3(a: tuple[float, float, float], b: tuple[float, float, float]) -> float:
    return a[0] * b[0] + a[1] * b[1] + a[2] * b[2]


def rigid_at(values: list[float], index: int) -> str:
    """What a rigid transform at `index` would have to look like, if it is one.

    Two layouts are tried, because a title uses one of them and the numbers do
    not say which:

      * three rows of three with the translation beside them in a fourth column,
        which is what a 3x4 row-major matrix with a translation column is;
      * three columns of three with the translation after them, which is the
        same matrix stored the other way round.

    The same twelve floats satisfy one layout and not the other -- a transform is
    not symmetric -- so which one holds is a real answer rather than a coin toss.
    It is still reported by name, because the translation follows the layout: read
    a rows layout as columns and the translation lands somewhere plausible and
    wrong, in a way the screen will not show. Which one a title uses is a fact
    about the title, and the report says which reading it used rather than
    leaving a reader to assume.
    """
    if index + 12 > len(values):
        return ""
    candidates = (
        (
            "rows of three with the translation in a fourth column",
            [tuple(values[index + r * 4 : index + r * 4 + 3]) for r in range(3)],
            (values[index + 3], values[index + 7], values[index + 11]),
        ),
        (
            "columns of three with the translation after them",
            [tuple(values[index + c * 3 : index + c * 3 + 3]) for c in range(3)],
            (values[index + 9], values[index + 10], values[index + 11]),
        ),
    )
    found: list[str] = []
    for name, basis, translation in candidates:
        if not all(abs(length3(b) - 1.0) <= UNIT_TOLERANCE for b in basis):
            continue
        if any(abs(dot3(basis[a], basis[b])) > UNIT_TOLERANCE for a, b in ((0, 1), (0, 2), (1, 2))):
            continue
        found.append(f"{name}, translation {translation}")
    if not found:
        return ""
    return "a rigid transform -- " + "; and also ".join(found)


def clusters(differing: list[int], gap: int = 4) -> list[tuple[int, int]]:
    """The differing floats, grouped into runs no more than `gap` apart.

    A block holds many things that change per tick -- a pose, a colour, a counter
    -- and they are not all one thing. Grouping them says how many separate runs
    moved, which is what decides whether one range can be named at all. Gaps up to
    `gap` floats are treated as one run because a matrix's rows are interleaved
    with the bytes around them and a stride would otherwise split a transform
    into three.
    """
    if not differing:
        return []
    runs: list[tuple[int, int]] = []
    start = previous = differing[0]
    for index in differing[1:]:
        if index - previous > gap:
            runs.append((start, previous))
            start = index
        previous = index
    runs.append((start, previous))
    return runs


def rigid_runs(differing: list[int], values: list[float]) -> list[tuple[int, int]]:
    """Of those runs, the ones that are a rigid transform where they start.

    Where the run *starts* is the whole test. How long it is says how much of the
    transform moved, which is usually all of it and is not what makes it one, and
    looking for a second transform inside the run would find one only by accident
    of its length.
    """
    return [run for run in clusters(differing) if rigid_at(values, run[0])]


def main(argv: list[str] | None = None) -> int:
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("before", type=Path, help="the dump taken first")
    parser.add_argument("after", type=Path, help="the dump taken a tick later")
    parser.add_argument(
        "--max-floats",
        type=int,
        default=24,
        help="how many differing floats to print; the rest is counted, not hidden",
    )
    parser.add_argument(
        "--ranges",
        action="store_true",
        help="report the runs of moving floats as byte ranges, and refuse to choose "
        "between them when there is more than one",
    )
    args = parser.parse_args(argv)

    for path in (args.before, args.after):
        if not path.is_file():
            print(f"refused: no dump at {path}", file=sys.stderr)
            return 2
    first = args.before.read_bytes()
    second = args.after.read_bytes()
    if len(first) != len(second):
        print(f"refused: the dumps differ in size: {len(first)} and {len(second)} bytes")
        return 2

    before = floats(first)
    after = floats(second)
    differing = [i for i, (a, b) in enumerate(zip(before, after)) if a != b]
    print(f"== {len(differing)} of {len(before)} floats differ over {len(first)} bytes")
    if not differing:
        print("== nothing moved, so nothing here is a per-tick pose")
        return 0

    shown = 0
    for index in differing:
        shape = rigid_at(after, index)
        note = f"  <- {shape}" if shape else ""
        print(
            f"   float {index:4d} (byte {index * 4:5d}): {before[index]:12.6g} -> "
            f"{after[index]:12.6g}{note}"
        )
        shown += 1
        if shown >= args.max_floats:
            break
    if shown < len(differing):
        print(f"   ... and {len(differing) - shown} more")

    # Where a rigid transform starts, and how far the run of moving floats goes:
    # the pose is a range, and a range is what a blend has to write.
    starts = sorted({i for i in differing if rigid_at(after, i)})
    if starts:
        print(f"== a rigid transform starts at float {starts[0]} (byte {starts[0] * 4})")
        end = max(i for i in differing)
        print(
            f"== and the moving floats run to float {end} (byte {end * 4}), so the pose's "
            f"range is at most {end - starts[0] + 1} floats"
        )
    else:
        print("== no rigid transform among the floats that moved")

    if args.ranges:
        runs = clusters(differing)
        print(f"== {len(runs)} separate runs of moving floats")
        for first, last in runs:
            note = "  <- a rigid transform at its start" if rigid_at(after, first) else ""
            print(f"   floats {first}..{last} (bytes {first * 4}..{last * 4 + 3}){note}")
        if len(runs) == 1:
            print(
                f"== so one range covers everything that moved: bytes {runs[0][0] * 4}.."
                f"{runs[0][1] * 4 + 3}, {runs[0][1] - runs[0][0] + 1} floats"
            )
        elif not runs:
            print("== nothing moved, so there is no range to write")
        else:
            # Naming the largest run and calling it the pose is the mistake the
            # mechanism being retired made, so this refuses instead: a blend that
            # writes one of several ranges needs to be told which, by whoever owns
            # the title's uniform names.
            print(
                f"== refused: {len(runs)} ranges moved and this does not choose between "
                "them. Each is listed above with its bytes."
            )
    return 0


if __name__ == "__main__":
    raise SystemExit(main(sys.argv[1:]))
