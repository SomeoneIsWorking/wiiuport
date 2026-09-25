#!/usr/bin/env python3
"""Convert a Wii U executable (RPX) to a plain ELF for Ghidra's own loader.

Maintainer RE tool. The RPX is the player's: extract it with the
wiiuport_title_files executable into the gitignored build/re/, convert it
there, and import the ELF into the Ghidra project under build/ghidra/.
"""

from __future__ import annotations

import argparse
import sys
from pathlib import Path

from wiiuport.rpx import NotAnRpx, to_elf


def main(argv: list[str] | None = None) -> int:
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("rpx", type=Path)
    parser.add_argument("elf", type=Path)
    args = parser.parse_args(argv)
    try:
        elf = to_elf(args.rpx.read_bytes())
    except (OSError, NotAnRpx) as failure:
        print(f"{args.rpx}: {failure}", file=sys.stderr)
        return 1
    args.elf.write_bytes(elf)
    print(f"{args.rpx} -> {args.elf} ({len(elf)} bytes)")
    return 0


if __name__ == "__main__":
    sys.exit(main())
