"""Press buttons on the running title over its control channel.

This is what makes a maintainer run more than an observation: a run nobody
can press a button in never leaves the title screen, and everything measured
from it describes an unattended screen rather than the game.
"""

from __future__ import annotations

import json
import urllib.error
import urllib.parse
import urllib.request

from wiiuport.control import DEFAULT_PORT, ControlUnavailable


def send_input(port: int = DEFAULT_PORT, timeout: float = 5.0, **parameters: object) -> dict:
    """Apply one input request, refusing by reason rather than returning empty.

    The runtime answers 400 for a parameter it does not know, so a typo is
    reported here instead of being counted as a press that was sent.
    """
    query = urllib.parse.urlencode({k: v for k, v in parameters.items() if v is not None})
    url = f"http://127.0.0.1:{port}/input?{query}"
    request = urllib.request.Request(url, method="POST", data=b"")
    try:
        with urllib.request.urlopen(request, timeout=timeout) as response:
            return json.loads(response.read().decode("utf-8"))
    except urllib.error.HTTPError as refused:
        body = refused.read().decode("utf-8", "replace").strip()
        raise ControlUnavailable(f"{url} was refused ({refused.code}): {body}") from refused
    except urllib.error.URLError as unreachable:
        raise ControlUnavailable(
            f"{url} did not answer ({unreachable.reason}). The runtime is not running, "
            "or was started without WIIUPORT_CONTROL_PORT."
        ) from unreachable


def press(button: str, port: int = DEFAULT_PORT, reads: int = 8) -> dict:
    return send_input(port=port, press=button, reads=reads)


def left_stick(x: float, y: float, port: int = DEFAULT_PORT) -> dict:
    return send_input(port=port, leftx=x, lefty=y)


def release(port: int = DEFAULT_PORT) -> dict:
    return send_input(port=port, release=1)


def arm_replay(port: int = DEFAULT_PORT, timeout: float = 5.0) -> dict:
    """Arm one replay. Not input, but the same one-shot verb over the same
    channel, and kept beside it so a tool needs one import to drive a run."""
    url = f"http://127.0.0.1:{port}/replay"
    request = urllib.request.Request(url, method="POST", data=b"")
    try:
        with urllib.request.urlopen(request, timeout=timeout) as response:
            return json.loads(response.read().decode("utf-8"))
    except urllib.error.URLError as unreachable:
        raise ControlUnavailable(f"{url} did not answer ({unreachable.reason})") from unreachable
