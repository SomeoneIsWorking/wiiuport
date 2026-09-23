"""Press from the title's front end into the world, the one way every driven
run gets there.

Alternating A and Plus covers a logo that takes either, a file select that
takes A, and a start prompt that takes Plus. The alternation can end on the
Plus that opens the pause screen, where the camera holds still, so B backs out
of it last: in the field B only swings the sword.
"""

from __future__ import annotations

import time
from collections.abc import Callable

from .drive import press, release

PRESSES = 12
INTERVAL_SECONDS = 8
SETTLE_SECONDS = 3


def press_into_world(
    port: int,
    *,
    presses: int = PRESSES,
    interval: float = INTERVAL_SECONDS,
    still_running: Callable[[], bool] = lambda: True,
    after_press: Callable[[int], None] = lambda _index: None,
    sleep: Callable[[float], None] = time.sleep,
) -> int:
    """Presses through the front end and backs out of a pause screen.

    Returns the presses made: fewer than asked when the runtime exited, which
    the caller must treat as not having reached the world."""
    made = 0
    for index in range(presses):
        if not still_running():
            return made
        press("a" if index % 2 == 0 else "plus", port=port)
        made += 1
        sleep(interval)
        after_press(index)
    release(port=port)
    press("b", port=port)
    sleep(SETTLE_SECONDS)
    return made
