"""The client must require exactly what the channel reports.

`wiiuport.control.read_counters` refuses anything that is not JSON carrying every field in
`Counters`. That strictness is the point: a counters read that answers a plausible subset reads a
number that means something else. But strictness in one direction only is a trap -- the field list
is written by hand on both sides, and when one side loses a field the other keeps requiring it,
every tool that probes the channel for liveness fails and the failure reads as "the channel never
opened" rather than as "the client and the server disagree".

That is not hypothetical: the host-side interpolation's counters were deleted from the channel and
this test's subject is what noticed, one driven run later. So the two lists are compared here, by
reading the channel's own `countersJson` rather than by a hand-copied list of its own.
"""

from __future__ import annotations

import re
from pathlib import Path

import pytest

from wiiuport.control import Counters

ROOT = Path(__file__).resolve().parents[1]
CHANNEL = ROOT / "src/wiiuport/control/ControlChannel.cpp"


def reported_counter_fields() -> set[str]:
    """The field names `countersJson` writes, read from the source that writes them."""
    source = CHANNEL.read_text(encoding="utf-8")
    start = source.index("std::string ControlChannel::countersJson()")
    body = source[start : source.index("\n}\n", start)]
    return set(re.findall(r'\\"([A-Za-z]+)\\":', body))


def test_channel_source_is_present() -> None:
    """A missing source would make the comparison below vacuous, and a vacuous pass is not a check."""
    assert CHANNEL.is_file(), f"the channel source is not where this test looks: {CHANNEL}"
    assert reported_counter_fields(), "countersJson was read and named no field"


def test_client_requires_exactly_what_the_channel_reports() -> None:
    required = set(Counters.__annotations__)
    reported = reported_counter_fields()
    assert required == reported, (
        "the counters' two field lists have drifted apart, and every tool that probes the channel "
        "for liveness fails on the disagreement rather than saying so. "
        f"required but not reported: {sorted(required - reported)}; "
        f"reported but not required: {sorted(reported - required)}"
    )


def test_render_names_only_fields_the_dataclass_has() -> None:
    """A rendered sentence naming a field the dataclass dropped is an AttributeError at print time,
    in a run that has already cost nine minutes."""
    import inspect

    source = inspect.getsource(Counters.render)
    named = set(re.findall(r"self\.(\w+)", source))
    assert named <= set(Counters.__annotations__), (
        f"render() names fields the dataclass does not have: {sorted(named - set(Counters.__annotations__))}"
    )
    with pytest.raises(AttributeError):
        # The negative first: a field that is not there must not resolve, so the assertion above
        # is testing something that can fail.
        Counters.render(object())  # type: ignore[arg-type]
