"""Client for the running product's control channel.

This is how a tool asks the runtime what it is doing while it runs, instead of
launching it and reading its log afterwards. A log-scraping loop can only ask
what the script already knew to ask, and cannot ask anything of a run in
progress.
"""

from __future__ import annotations

import json
import urllib.error
import urllib.request
from dataclasses import dataclass

DEFAULT_PORT = 21337
"""The port maintainer tools use. The product opens none unless one is
configured, so this is a convention between tools, not a default the product
carries."""


class ControlUnavailable(RuntimeError):
    """The channel did not answer, with the reason named."""


@dataclass(frozen=True)
class Counters:
    """What the runtime reports about its own work, with denominators."""

    framesObserved: int
    framesRefusedIncomplete: int
    displayListsSeen: int
    uniformAssembliesSeen: int
    lastFrameDisplayLists: int
    lastFrameUniformAssemblies: int
    lastFrameBytes: int

    @property
    def recorded_anything(self) -> bool:
        return self.framesObserved > 0 and self.displayListsSeen > 0

    def render(self) -> str:
        return (
            f"frames {self.framesObserved} (refused incomplete "
            f"{self.framesRefusedIncomplete}), display lists {self.displayListsSeen}, "
            f"uniform assemblies {self.uniformAssembliesSeen}; last frame held "
            f"{self.lastFrameDisplayLists} lists and "
            f"{self.lastFrameUniformAssemblies} assemblies in {self.lastFrameBytes} bytes"
        )


def read_counters(port: int = DEFAULT_PORT, timeout: float = 2.0) -> Counters:
    """Read /counters, refusing by reason rather than returning empty."""
    url = f"http://127.0.0.1:{port}/counters"
    try:
        with urllib.request.urlopen(url, timeout=timeout) as response:
            payload = json.loads(response.read().decode("utf-8"))
    except urllib.error.URLError as unreachable:
        raise ControlUnavailable(
            f"{url} did not answer ({unreachable.reason}). The runtime is not running, "
            "or was started without WIIUPORT_CONTROL_PORT."
        ) from unreachable
    except json.JSONDecodeError as malformed:
        raise ControlUnavailable(f"{url} answered something that is not JSON: {malformed}") from (
            malformed
        )
    missing = {field for field in Counters.__annotations__} - set(payload)
    if missing:
        raise ControlUnavailable(
            f"{url} answered without {sorted(missing)}, so the runtime and this client "
            "disagree about what a counter set is"
        )
    return Counters(**{field: int(payload[field]) for field in Counters.__annotations__})
