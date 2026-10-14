"""Press A through the title's front end until the stage it plays is no longer a front-end one.

The stage name is read rather than waited on: the front end takes a varying number of presses, and a
press is only made once the title has run a second of its logic since the last.
"""

from __future__ import annotations

from collections.abc import Callable

from .control import request_bytes, wait_game_frames
from .drive import press

# dComIfG_play_c's start stage name, at play + 0x5134.
START_STAGE = 0x1046F0B0 + 0x5134
# Title screen, its sea backdrop, name entry and the ending's sea.
FRONT_END = frozenset({"", "sea_T", "Name", "ENDumi"})
PRESSES = 60
FRAMES_PER_PRESS = 60


def current_stage(port: int) -> str:
    raw = request_bytes("GET", f"/memory?address={START_STAGE:#x}&size=8", port, 5)
    return raw.split(b"\0")[0].decode("latin-1")


def press_into_world(
    port: int,
    *,
    presses: int = PRESSES,
    still_running: Callable[[], bool] = lambda: True,
    read_stage: Callable[[int], str] = current_stage,
    wait_frames: Callable[[int, int], None] = lambda frames, port: wait_game_frames(
        frames, port=port
    ),
) -> str | None:
    """The stage reached, or None when the runtime exited or the presses ran out first."""
    for _ in range(presses):
        if not still_running():
            return None
        press("a", port=port)
        wait_frames(FRAMES_PER_PRESS, port)
        stage = read_stage(port)
        if stage not in FRONT_END:
            return stage
    return None
