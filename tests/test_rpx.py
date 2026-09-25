"""An RPX converted to a plain ELF: sections inflated, in place, and refused when malformed."""

from __future__ import annotations

import struct
import zlib

import pytest
from wiiuport.rpx import (
    RPL_FILE_TYPE,
    SHF_RPL_ZLIB,
    SHT_NOBITS,
    NotAnRpx,
    to_elf,
)

_HEADER = struct.Struct(">16sHHIIIIIHHHHHH")
_SECTION = struct.Struct(">IIIIIIIIII")
TEXT = bytes(range(64)) * 4
DATA = b"data kept uncompressed"


def _rpx(text_size_claimed: int | None = None, file_type: int = RPL_FILE_TYPE) -> bytes:
    """A null section, a compressed text section, raw data, and bss."""
    compressed = struct.pack(">I", text_size_claimed or len(TEXT)) + zlib.compress(TEXT)
    body = bytearray(_HEADER.size)
    text_offset = len(body)
    body += compressed
    data_offset = len(body)
    body += DATA
    body += b"\0" * (-len(body) % 4)
    sections = [
        (0, 0, 0, 0, 0, 0, 0, 0, 0, 0),
        (1, 1, 0x6 | SHF_RPL_ZLIB, 0x02000000, text_offset, len(compressed), 0, 0, 32, 0),
        (2, 1, 0x3, 0x10000000, data_offset, len(DATA), 0, 0, 4, 0),
        (3, SHT_NOBITS, 0x3, 0x10001000, 0, 0x800, 0, 0, 64, 0),
    ]
    header = _HEADER.pack(
        b"\x7fELF\x01\x02\x01\xca\xfe" + b"\0" * 7,
        file_type,
        20,
        1,
        0x02000000,
        0,
        len(body),
        0,
        52,
        0,
        0,
        40,
        len(sections),
        0,
    )
    body[: _HEADER.size] = header
    for section in sections:
        body += _SECTION.pack(*section)
    return bytes(body)


def _read(elf: bytes) -> tuple[tuple, list[tuple]]:
    header = _HEADER.unpack_from(elf)
    offset, count = header[6], header[12]
    return header, [_SECTION.unpack_from(elf, offset + index * 40) for index in range(count)]


def test_every_section_is_inflated_where_the_loader_places_it() -> None:
    header, sections = _read(to_elf(_rpx()))
    elf = to_elf(_rpx())
    assert header[1] == 2, "an ordinary executable"
    assert header[0][7:9] == b"\0\0", "of the System V ABI"
    text, data, bss = sections[1], sections[2], sections[3]
    assert elf[text[4] : text[4] + text[5]] == TEXT
    assert text[2] & SHF_RPL_ZLIB == 0 and text[3] == 0x02000000
    assert text[4] % 32 == 0, "at its alignment"
    assert elf[data[4] : data[4] + data[5]] == DATA
    assert bss[5] == 0x800 and bss[1] == SHT_NOBITS, "bss keeps its size and holds no bytes"


def test_a_file_that_is_not_an_rpx_is_refused() -> None:
    with pytest.raises(NotAnRpx, match="not an ELF"):
        to_elf(b"PK\x03\x04" + b"\0" * 60)
    with pytest.raises(NotAnRpx, match="file type"):
        to_elf(_rpx(file_type=2))


def test_a_section_that_inflates_to_another_size_than_it_names_is_refused() -> None:
    with pytest.raises(NotAnRpx, match="inflated"):
        to_elf(_rpx(text_size_claimed=len(TEXT) + 1))
