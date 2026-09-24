"""Drive the runtime offscreen, silent, and isolated, for maintainer evidence.

This exists because the product launcher must never be used to diagnose,
measure, or smoke-test agent work: a windowed, audible run seizes the desktop
that belongs to the person in front of it.

Three isolations, all deliberate:

* **Display.** Offscreen, one of two ways. `Display.GPU` (the default) runs the
  product inside gamescope's headless backend, whose Xwayland offers DRI3, so
  Vulkan renders and presents on the real GPU. `Display.XVFB` is a dedicated
  Xvfb server, kept for tools that screenshot the X root window. Xvfb has no
  DRI3, and the runtime then renders on llvmpipe: measured, every headless
  timing taken under it was software rasterisation. `rendered_on()` reads which
  device the runtime actually chose, so that cannot pass unnoticed again.
  `Display.WAYLAND` runs the product as the session of a virtual KWin, so it
  opens its window through SDL's Wayland backend rather than any X server.
* **User data.** The runtime honours ``XDG_DATA_HOME``/``XDG_CONFIG_HOME``/
  ``XDG_CACHE_HOME`` on Linux, so a run gets its own directories and can never
  write to, or read stale state from, the operator's own installation.
* **Audio.** The isolated configuration names no output device, so a run
  nobody is watching makes no sound.

Processes are tracked and terminated **by the PID captured at launch**. This
machine runs sibling agents using the same binary name, so a name-based kill
would end another agent's run mid-gate, and can match the shell running the
command.
"""

from __future__ import annotations

import os
import shlex
import shutil
import signal
import subprocess
import time
from collections.abc import Iterator
from contextlib import contextmanager
from dataclasses import dataclass, field
from enum import Enum, IntEnum
from pathlib import Path
from typing import Self

from .paths import Layout


class Display(Enum):
    """Where an offscreen run presents. See the module docstring."""

    GPU = "gpu"
    XVFB = "xvfb"
    WAYLAND = "wayland"


TITLE_OUTPUT_SIZE = (1920, 1080)
"""The title's own output size, which an offscreen display defaults to."""


def gamescope_args(size: tuple[int, int]) -> tuple[str, ...]:
    """gamescope's headless backend at `size`. It parents the product and
    hands it its own Xwayland display, so no display is started separately and
    nothing reaches the operator's compositor."""
    width, height = size
    return ("--backend", "headless", "-W", str(width), "-H", str(height), "--")


def kwin_args(size: tuple[int, int], socket: str) -> tuple[str, ...]:
    """A virtual KWin at `size` on its own socket. The command follows as one
    `--exit-with-session` argument, which KWin splits as a shell would, runs
    with WAYLAND_DISPLAY naming that socket, and exits with, passing its code
    on."""
    width, height = size
    return (
        "--virtual",
        "--no-lockscreen",
        "--socket",
        socket,
        "--width",
        str(width),
        "--height",
        str(height),
        "--exit-with-session",
    )


SOFTWARE_RENDERERS = ("llvmpipe", "lavapipe", "softpipe", "swiftshader")


class LogType(IntEnum):
    """The runtime's log-type bit positions, as `LogType` in the fork.

    Only the ones this harness switches on are listed; the runtime owns the
    full set. `logflag` is a bitmask over these positions, so GX2 (1) is 2.
    """

    GX2 = 1
    UNIFORM_CAPTURE = 27
    DISPLAY_LIST_CAPTURE = 28


def log_flags(*types: LogType) -> int:
    """The `logflag` value that enables exactly these types, and nothing else."""
    mask = 0
    for log_type in types:
        mask |= 1 << int(log_type)
    return mask


GAMEPAD_PROFILE = """<?xml version="1.0" encoding="UTF-8"?>
<emulated_controller>
	<type>Wii U GamePad</type>
</emulated_controller>
"""
"""An emulated gamepad with no device behind it. See _write_gamepad_profile."""

SETTINGS_TEMPLATE = """<?xml version="1.0" encoding="UTF-8"?>
<content>
    <logflag>{logflag}</logflag>
    <check_update>false</check_update>
    <use_discord_presence>false</use_discord_presence>
    <gp_download>false</gp_download>
    <play_boot_sound>false</play_boot_sound>
    <disable_screensaver>false</disable_screensaver>
    <console_language>1</console_language>
    <Graphic>
        <api>1</api>
        <VSync>0</VSync>
        <GX2DrawdoneSync>true</GX2DrawdoneSync>
    </Graphic>
    <Audio>
        <api>0</api>
        <TVChannels>1</TVChannels>
        <TVVolume>0</TVVolume>
        <TVDevice></TVDevice>
        <PadDevice></PadDevice>
    </Audio>
</content>
"""
"""Graphics api 1 is Vulkan, the backend interpolation substitutes into.
Both audio devices are left unnamed on purpose: an unwatched run is silent."""


class HeadlessError(RuntimeError):
    """The offscreen environment could not be established, named precisely."""


@dataclass
class RunResult:
    """What a driven run produced. Absence of a symptom is not a result."""

    exit_code: int | None
    timed_out: bool
    seconds: float
    log_path: Path
    session_dir: Path

    @property
    def reached_the_binary(self) -> bool:
        return self.exit_code is not None or self.timed_out


@dataclass
class HeadlessSession:
    """One isolated offscreen environment, reused at a fixed scratch path."""

    layout: Layout
    display: int = 99
    activity: str = "headless"
    logflag: int = 0
    """Which runtime log types to enable, from `log_flags`. Zero keeps a run
    quiet; a capture run switches on exactly what it intends to read."""

    display_server: Display = Display.GPU
    output_size: tuple[int, int] = TITLE_OUTPUT_SIZE
    """The offscreen display's size: the title's by default, a player's
    display's to measure what presenting at it costs."""

    runtime_env: dict[str, str] = field(default_factory=dict)
    """Runtime-specific overrides layered over the isolated environment. They
    cannot displace the isolation itself, which is applied last."""

    _xvfb: subprocess.Popen[bytes] | None = field(default=None, init=False, repr=False)

    @property
    def session_dir(self) -> Path:
        return self.layout.activity_dir(self.activity)

    @property
    def config_home(self) -> Path:
        return self.session_dir / "config"

    @property
    def data_home(self) -> Path:
        return self.session_dir / "data"

    @property
    def cache_home(self) -> Path:
        return self.session_dir / "cache"

    def environment(self) -> dict[str, str]:
        env = dict(os.environ)
        env.update(self.runtime_env)
        # Applied after the caller's overrides: isolation is the one part of
        # this environment a caller must not be able to reach around.
        env.pop("WAYLAND_DISPLAY", None)
        if self.display_server is Display.XVFB:
            env["DISPLAY"] = f":{self.display}"
        elif self.display_server is Display.WAYLAND:
            # KWin names its socket to the session it starts; no X server is
            # offered, and SDL is held to Wayland so it cannot fall back to X.
            env.pop("DISPLAY", None)
            env["SDL_VIDEO_DRIVER"] = "wayland"
        else:
            # gamescope sets its own Xwayland display for the product; an
            # inherited one would point it at the operator's desktop.
            env.pop("DISPLAY", None)
        env["XDG_CONFIG_HOME"] = str(self.config_home)
        env["XDG_DATA_HOME"] = str(self.data_home)
        env["XDG_CACHE_HOME"] = str(self.cache_home)
        env["XDG_SESSION_TYPE"] = "wayland" if self.display_server is Display.WAYLAND else "x11"
        return env

    def prepare(self, keys_source: Path | None = None, save_source: Path | None = None) -> None:
        """Create the isolated directories and write the offscreen settings."""
        for directory in (self.config_home, self.data_home, self.cache_home):
            directory.mkdir(parents=True, exist_ok=True)
        (self.config_home / "Cemu").mkdir(exist_ok=True)
        (self.data_home / "Cemu").mkdir(exist_ok=True)
        (self.config_home / "Cemu" / "settings.xml").write_text(
            SETTINGS_TEMPLATE.format(logflag=self.logflag)
        )
        self._write_gamepad_profile()
        if keys_source is not None:
            self._link_keys(keys_source)
        if save_source is not None:
            self._copy_save(save_source)

    def _write_gamepad_profile(self) -> None:
        """Give player one a gamepad, because the title only reads one that exists.

        With no profile the VPAD HLE finds no emulated controller, returns
        empty samples and never calls into the controller at all -- so the
        runtime's injection point is never reached and a press cannot arrive.
        Measured: a driven run reported 12 presses queued and 0 gamepad reads.

        The profile attaches no physical device deliberately. It exists so the
        emulated gamepad exists; everything it reports comes from the runtime.
        """
        profiles = self.config_home / "Cemu" / "controllerProfiles"
        profiles.mkdir(parents=True, exist_ok=True)
        (profiles / "controller0.xml").write_text(GAMEPAD_PROFILE)

    def _link_keys(self, keys_source: Path) -> None:
        """Link, never copy, the operator's title keys into the session.

        Keys are the operator's and are derived restricted data. They live in
        gitignored scratch only as a link, so nothing can later mistake a copy
        for a project file.
        """
        if not keys_source.is_file():
            raise HeadlessError(
                f"title keys were requested but {keys_source} does not exist; "
                "a run without them cannot open an encrypted disc image"
            )
        target = self.data_home / "Cemu" / "keys.txt"
        if target.is_symlink() or target.exists():
            target.unlink()
        target.symlink_to(keys_source)

    def _copy_save(self, save_source: Path) -> None:
        """Copy, never link, the operator's save into the session's own mlc.

        A run writes to its save as it plays. Linking would put the operator's
        own progress behind a driven run that presses buttons at random, and
        the first overwritten quest log would be theirs. The copy is in
        gitignored scratch and is the session's to ruin.
        """
        if not save_source.is_dir():
            raise HeadlessError(
                f"a save was requested but {save_source} is not a directory; "
                "a run without one stops at the name-entry keyboard"
            )
        target = self.data_home / "Cemu" / "mlc01" / "usr" / "save" / "00050000" / save_source.name
        if target.exists():
            shutil.rmtree(target)
        target.parent.mkdir(parents=True, exist_ok=True)
        shutil.copytree(save_source, target)

    def rendered_on(self) -> str:
        """The Vulkan device the runtime reported choosing, from its own log.

        Refuses rather than guessing when the line is absent: a run whose
        device is unknown cannot be evidence about any device."""
        log = self.data_home / "Cemu" / "log.txt"
        if log.is_file():
            for line in log.read_text(errors="replace").splitlines():
                if "Using GPU:" in line:
                    return line.split("Using GPU:", 1)[1].strip()
        raise HeadlessError(f"{log} names no Vulkan device; the renderer never initialised")

    def is_software_rendered(self) -> bool:
        device = self.rendered_on().lower()
        return any(name in device for name in SOFTWARE_RENDERERS)

    def command_for(self, command: list[str]) -> list[str]:
        """The command as launched: under gamescope for a GPU display."""
        if self.display_server is Display.XVFB:
            return command
        if self.display_server is Display.WAYLAND:
            socket = f"wiiuport-{self.display}"
            return ["kwin_wayland", *kwin_args(self.output_size, socket), shlex.join(command)]
        return ["gamescope", *gamescope_args(self.output_size), *command]

    def start_display(self) -> None:
        if self.display_server is Display.WAYLAND:
            if shutil.which("kwin_wayland") is None:
                raise HeadlessError(
                    "kwin_wayland is not installed, so a run cannot open a Wayland window "
                    "offscreen. Install it:\n  sudo dnf install kwin"
                )
            return
        if self.display_server is Display.GPU:
            if shutil.which("gamescope") is None:
                raise HeadlessError(
                    "gamescope is not installed, so a run cannot render offscreen on the "
                    "GPU. Install it:\n  sudo dnf install gamescope"
                )
            return
        if shutil.which("Xvfb") is None:
            raise HeadlessError(
                "Xvfb is not installed, so a run would have to open a window on "
                "the operator's desktop. Install it:\n"
                "  sudo dnf install xorg-x11-server-Xvfb"
            )
        self._xvfb = subprocess.Popen(
            [
                "Xvfb",
                f":{self.display}",
                "-screen",
                "0",
                f"{self.output_size[0]}x{self.output_size[1]}x24",
                "-nolisten",
                "tcp",
            ],
            stdout=subprocess.DEVNULL,
            stderr=subprocess.DEVNULL,
            start_new_session=True,
        )
        deadline = time.monotonic() + 10.0
        while time.monotonic() < deadline:
            if self._xvfb.poll() is not None:
                raise HeadlessError(
                    f"Xvfb on display :{self.display} exited immediately with "
                    f"code {self._xvfb.returncode}; the display number may be in use"
                )
            if Path(f"/tmp/.X11-unix/X{self.display}").exists():
                return
            time.sleep(0.1)
        self.stop_display()
        raise HeadlessError(f"Xvfb did not provide display :{self.display} within 10s")

    def stop_display(self) -> None:
        """Terminate the display by the PID captured at launch, never by name."""
        if self._xvfb is None:
            return
        _terminate(self._xvfb)
        self._xvfb = None

    @contextmanager
    def launch(self, command: list[str]) -> Iterator[subprocess.Popen[bytes]]:
        """Start the command and hand it back while it runs.

        This is what lets a tool interrogate a running product instead of
        launching it and reading its log afterwards. The process is always
        terminated by the PID captured here on the way out, never by name:
        other agents and the operator run the same binary.
        """
        log_path = self.session_dir / "run.log"
        with log_path.open("wb") as log:
            process = subprocess.Popen(
                self.command_for(command),
                env=self.environment(),
                stdout=log,
                stderr=subprocess.STDOUT,
                start_new_session=True,
            )
            try:
                yield process
            finally:
                if process.poll() is None:
                    _terminate(process)

    def run(self, command: list[str], *, timeout_seconds: float) -> RunResult:
        """Run a bounded, offscreen command and always return what happened."""
        log_path = self.session_dir / "run.log"
        started = time.monotonic()
        with log_path.open("wb") as log:
            process = subprocess.Popen(
                self.command_for(command),
                env=self.environment(),
                stdout=log,
                stderr=subprocess.STDOUT,
                start_new_session=True,
            )
            try:
                exit_code: int | None = process.wait(timeout=timeout_seconds)
                timed_out = False
            except subprocess.TimeoutExpired:
                _terminate(process)
                exit_code, timed_out = None, True
        return RunResult(
            exit_code=exit_code,
            timed_out=timed_out,
            seconds=time.monotonic() - started,
            log_path=log_path,
            session_dir=self.session_dir,
        )

    def __enter__(self) -> Self:
        self.start_display()
        return self

    def __exit__(self, *_: object) -> None:
        self.stop_display()


def _own_process_group(pid: int) -> int | None:
    """The group of a process we launched, but only if it is not our own.

    Everything this module launches is given ``start_new_session=True`` so it
    leads its own group and can be signalled as a unit. If a group id ever
    matches this process\'s, signalling it would kill the harness and whatever
    else shares the group — so the caller is told to signal the single PID
    instead. This is not defensive padding: an earlier version signalled a
    child\'s group before that child was given its own session, and killed the
    tool that launched it.
    """
    try:
        group = os.getpgid(pid)
    except (ProcessLookupError, PermissionError):
        return None
    return None if group == os.getpgrp() else group


def _signal(process: subprocess.Popen[bytes], sig: int) -> None:
    group = _own_process_group(process.pid)
    try:
        if group is None:
            process.send_signal(sig)
        else:
            os.killpg(group, sig)
    except (ProcessLookupError, PermissionError):
        pass


def _terminate(process: subprocess.Popen[bytes]) -> None:
    """Stop a process we launched, by the PID captured at launch."""
    if process.poll() is not None:
        return
    _signal(process, signal.SIGTERM)
    try:
        process.wait(timeout=10)
        return
    except subprocess.TimeoutExpired:
        pass
    _signal(process, signal.SIGKILL)
    process.wait(timeout=10)
