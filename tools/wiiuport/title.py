"""Where the player's disc image comes from, and the refusal when it does not.

One owner, because more than one maintainer tool needs it and the refusal has
to keep saying the same thing: the image is never stored in this repository and
never guessed at.
"""

from __future__ import annotations

import os
from pathlib import Path

ENV_GAME = "WIIUPORT_GAME"


class TitleUnavailable(RuntimeError):
    """No usable disc image, with the reason named."""


def resolve_game(argument: Path | None) -> Path:
    """The explicit argument, else the environment. Never a guess."""
    game = argument or (Path(os.environ[ENV_GAME]) if ENV_GAME in os.environ else None)
    if game is None:
        raise TitleUnavailable(
            f"no disc image given. Pass --game or set {ENV_GAME}. This tool does not "
            "guess a path, and the image is never stored in this repository."
        )
    if not game.is_file():
        raise TitleUnavailable(f"{game} is not a file")
    return game


ENV_KEYS = "WIIUPORT_KEYS"


def resolve_keys(argument: Path | None) -> Path:
    """The title keys. Required: an encrypted disc image cannot be opened
    without them, and a run that silently proceeds without them hangs instead
    of failing."""
    keys = argument or (Path(os.environ[ENV_KEYS]) if ENV_KEYS in os.environ else None)
    if keys is None:
        raise TitleUnavailable(
            f"no title keys given. Pass --keys or set {ENV_KEYS}. They are the "
            "operator's and are never stored in this repository."
        )
    if not keys.is_file():
        raise TitleUnavailable(f"{keys} is not a file")
    return keys


ENV_SAVE = "WIIUPORT_SAVE"


def resolve_save(argument: Path | None) -> Path | None:
    """The operator's save for the title, or None when none was named.

    Optional, and absent is a legitimate answer: a run without one starts a
    new game and stops at the name-entry keyboard, which no button press can
    pass. What is never acceptable is pretending a run reached gameplay when
    it sat on that screen, so the tools that need gameplay say so themselves.

    A named save that does not exist is refused rather than ignored: a typo
    would otherwise read as "no save", which is the case this exists to avoid.
    """
    save = argument or (Path(os.environ[ENV_SAVE]) if ENV_SAVE in os.environ else None)
    if save is None:
        return None
    if not save.is_dir():
        raise TitleUnavailable(
            f"{save} is not a directory. A save is the title's save folder, the one "
            f"holding user/<account>; pass --save or set {ENV_SAVE}."
        )
    return save
