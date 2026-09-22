"""A real gamepad the kernel creates, for proving the host attaches one.

An automatic mapping that has only ever been seen reporting "no gamepad is
plugged in" is not known to work: the negative branch is the one that runs
with no pad on the desk. This creates a genuine input device through uinput,
so SDL discovers it through the same path a physical pad arrives on, hotplug
event included, and removes it again to prove the departure is noticed too.

It needs write access to /dev/uinput, which on this distribution means
membership of the `input` group. Without it the tool refuses by name rather
than skipping the check.
"""

from __future__ import annotations

import time
from pathlib import Path
from types import TracebackType
from typing import Self

from evdev import AbsInfo, InputDevice, UInput, ecodes, list_devices

UINPUT = Path("/dev/uinput")

# An Xbox-style pad: the layout SDL's gamepad database recognises, so the
# device arrives as a gamepad rather than an unmapped joystick.
VENDOR = 0x045E
PRODUCT = 0x028E
NAME = "wiiuport virtual gamepad"

_AXIS = AbsInfo(value=0, min=-32768, max=32767, fuzz=16, flat=128, resolution=0)
_TRIGGER = AbsInfo(value=0, min=0, max=255, fuzz=0, flat=0, resolution=0)
_HAT = AbsInfo(value=0, min=-1, max=1, fuzz=0, flat=0, resolution=0)

CAPABILITIES = {
    ecodes.EV_KEY: [
        ecodes.BTN_SOUTH,
        ecodes.BTN_EAST,
        ecodes.BTN_NORTH,
        ecodes.BTN_WEST,
        ecodes.BTN_TL,
        ecodes.BTN_TR,
        ecodes.BTN_SELECT,
        ecodes.BTN_START,
        ecodes.BTN_MODE,
        ecodes.BTN_THUMBL,
        ecodes.BTN_THUMBR,
    ],
    ecodes.EV_ABS: [
        (ecodes.ABS_X, _AXIS),
        (ecodes.ABS_Y, _AXIS),
        (ecodes.ABS_RX, _AXIS),
        (ecodes.ABS_RY, _AXIS),
        (ecodes.ABS_Z, _TRIGGER),
        (ecodes.ABS_RZ, _TRIGGER),
        (ecodes.ABS_HAT0X, _HAT),
        (ecodes.ABS_HAT0Y, _HAT),
    ],
}


class VirtualPadUnavailable(RuntimeError):
    """The kernel would not create the device, with the reason named."""


class VirtualPad:
    """A gamepad that exists while this object does."""

    def __init__(self, name: str = NAME) -> None:
        self._name = name
        self._device: UInput | None = None

    def __enter__(self) -> Self:
        self.plug()
        return self

    def __exit__(
        self,
        exc_type: type[BaseException] | None,
        exc: BaseException | None,
        traceback: TracebackType | None,
    ) -> None:
        self.unplug()

    def plug(self) -> None:
        if not UINPUT.exists():
            raise VirtualPadUnavailable(
                f"{UINPUT} does not exist, so no gamepad can be created. Load the uinput "
                "module (sudo modprobe uinput) and try again."
            )
        try:
            self._device = UInput(
                CAPABILITIES, name=self._name, vendor=VENDOR, product=PRODUCT, version=0x110
            )
        except PermissionError as denied:
            raise VirtualPadUnavailable(
                f"{UINPUT} is not writable by this user, so no gamepad can be created. "
                "Membership of the `input` group grants it."
            ) from denied

    def unplug(self) -> None:
        if self._device is not None:
            self._device.close()
            self._device = None

    def press(self, code: int, hold: float = 0.05) -> None:
        """One button press, so the device also produces events."""
        if self._device is None:
            raise VirtualPadUnavailable("the gamepad is not plugged in")
        self._device.write(ecodes.EV_KEY, code, 1)
        self._device.syn()
        time.sleep(hold)
        self._device.write(ecodes.EV_KEY, code, 0)
        self._device.syn()

    def is_visible(self) -> bool:
        """Whether the kernel is showing this device to other processes.

        Checked rather than assumed: a device the kernel created but has not
        published yet would make the host look at fault.
        """
        for path in list_devices():
            try:
                if InputDevice(path).name == self._name:
                    return True
            except OSError:
                continue
        return False
