"""The interpolation client must read what the runtime framed, or refuse."""

from __future__ import annotations

import struct

import pytest
from wiiuport.interpolation import RECORDINGS_MAGIC, Interpolation, parse_recordings

from wiiuport.control import ENV_CONTROL_PORT, ENV_INTERPOLATION, ControlUnavailable, runtime_env


def assembly(base: int, sources: list[int], floats: list[float]) -> bytes:
    return (
        struct.pack("=QQII", base, 0, 1, len(sources))
        + struct.pack(f"={len(sources)}I", *sources)
        + struct.pack("=I", len(floats))
        + struct.pack(f"={len(floats)}f", *floats)
    )


def snapshot(*frames: list[bytes]) -> bytes:
    body = RECORDINGS_MAGIC + struct.pack("=I", len(frames))
    for assemblies in frames:
        body += struct.pack("=II", 1, len(assemblies)) + b"".join(assemblies)
    return body


def test_consecutive_frames_decode_in_order():
    frames = parse_recordings(
        snapshot([assembly(0xAA, [1, 0x1000], [1.0, 2.0])], [assembly(0xBB, [], [3.0])])
    )
    assert len(frames) == 2
    assert frames[0].assemblies[0].baseHash == 0xAA
    assert frames[0].assemblies[0].sources == (1, 0x1000)
    assert frames[0].assemblies[0].floats == (1.0, 2.0)
    assert frames[1].assemblies[0].floats == (3.0,)


def test_a_truncated_snapshot_is_refused_rather_than_cut_short():
    body = snapshot([assembly(0xAA, [], [1.0, 2.0])])
    with pytest.raises(ControlUnavailable, match="truncated"):
        parse_recordings(body[:-2])


def test_trailing_bytes_are_refused():
    with pytest.raises(ControlUnavailable, match="after its last frame"):
        parse_recordings(snapshot([]) + b"\0")


def test_a_foreign_body_is_refused():
    with pytest.raises(ControlUnavailable, match="WIIUREC1"):
        parse_recordings(b"OTHERMAG" + bytes(4))


def report(ticks: int, interpolated: int, no_view: int) -> Interpolation:
    return Interpolation(
        enabled=True,
        ticks=ticks,
        framesInterpolated=interpolated,
        skipped={"noView": no_view, "cameraCut": 0},
        restoresRefused=0,
        restoresByCopy=interpolated,
        restoresByReplay=0,
        subresourcesRestored=interpolated * 4,
        shadowsCreated=4,
        notCopied={"subresources": 0, "texturesCreated": 0, "streamoutWrites": 0},
        restoreChecksCompleted=0,
        restoreChecksRefused=0,
        neighbourChecksCompleted=0,
        neighbourChecksRefused=0,
        neighbourChecksRestarted=0,
        phaseNanoseconds={"blendedReplay": interpolated * 1000},
        cutsByTurn=0,
        cutsByStep=0,
        objects={"blended": interpolated, "unmatched": 1},
        objectPartnersDerived=0,
        objectPartnersSearched=0,
        objectPartnersReidentified=0,
        objectReidentifyAttempts=0,
        objectNearestCandidates=0,
        objectPartnerCandidates=0,
        objectFramesEnded=0,
        objectFrameEndPlanningNanoseconds=0,
        objectPlanningBusyNanoseconds=0,
        objectSearchesDeferred=0,
        objectHeldPartnersDerived=0,
        objectValuesNotBlended=0,
        objectValuesAlternating=0,
        objectDrawsWritten=interpolated,
        objectReplaysDiverged=0,
        vertexDraws={"blended": interpolated, "noPartner": 2},
        runtimeDrawsReplaceable=interpolated * 3,
        runtimeDrawsReplaced=interpolated,
        vertexReplaysDiverged=0,
        vertexReplaysUnaligned=0,
        vertexBytesCopied=0,
        vertexCopyingNanoseconds=0,
        vertexBlendingNanoseconds=0,
        vertexWaitingNanoseconds=0,
        pacing={"guestFrames": ticks, "runtimeFrames": interpolated, "p50Us": 16667},
        viewFramesTracked=0,
        viewFramesLost=0,
        viewReseedsRun=0,
        viewReseedsFound=0,
        copiesSubmitted=interpolated,
        withheld={"presentation": 0},
        recordingSnapshots=0,
    )


def test_a_window_counts_only_what_happened_inside_it():
    window = report(100, 60, 40).since(report(30, 0, 30))
    assert (window.ticks, window.framesInterpolated) == (70, 60)
    assert window.skipped == {"noView": 10, "cameraCut": 0}
    assert window.objects == {"blended": 60, "unmatched": 0}
    assert window.vertexDraws == {"blended": 60, "noPartner": 0}
    assert (window.runtimeDrawsReplaced, window.runtimeDrawsReplaceable) == (60, 180)
    assert window.pacing == {"guestFrames": 100, "runtimeFrames": 60, "p50Us": 16667}, (
        "pacing is measured over its own window, so the later one is kept whole"
    )


def test_one_shot_tools_switch_continuous_off_and_the_rest_leave_it_on():
    assert runtime_env(1234) == {ENV_CONTROL_PORT: "1234", ENV_INTERPOLATION: "1"}
    assert runtime_env(1234, continuous=False)[ENV_INTERPOLATION] == "0"


def test_a_snapshot_is_the_one_armed_not_the_one_before(monkeypatch: pytest.MonkeyPatch):
    import types

    from wiiuport import control, interpolation

    # The runtime's completed count at each read: the armed snapshot
    # completes on the fourth.
    completed = iter([3, 3, 3, 4])
    latest = {"count": 3}

    def read(port: int) -> types.SimpleNamespace:
        latest["count"] = next(completed)
        return types.SimpleNamespace(recordingSnapshots=latest["count"])

    def request(method: str, path: str, port: int, timeout: float) -> bytes:
        return f"snapshot {latest['count']}".encode()

    monkeypatch.setattr(interpolation, "read_interpolation", read)
    monkeypatch.setattr(interpolation, "request_bytes", request)
    monkeypatch.setattr(control.time, "sleep", lambda _seconds: None)
    assert interpolation.take_recordings(4, port=1) == b"snapshot 4", (
        "the snapshot completed before arming is never read, however long the new one takes"
    )
