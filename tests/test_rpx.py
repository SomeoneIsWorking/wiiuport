"""An RPX converted to a plain ELF: sections inflated and linked, or refused when malformed."""

from __future__ import annotations

import struct
import zlib

import pytest
from wiiuport.rpx import RPL_FILE_TYPE, SHF_RPL_ZLIB, SHT_NOBITS, NotAnRpx, to_elf
from wiiuport.rpx_link import (
    R_PPC_ADDR16_HA,
    R_PPC_ADDR16_LO,
    R_PPC_REL24,
    SHT_RELA,
    SHT_RPL_IMPORTS,
    SHT_SYMTAB,
    LinkRefused,
)

_HEADER = struct.Struct(">16sHHIIIIIHHHHHH")
_SECTION = struct.Struct(">IIIIIIIIII")
_SYMBOL = struct.Struct(">IIIBBH")
_RELOCATION = struct.Struct(">IIi")
TEXT_ADDRESS, DATA_ADDRESS, IMPORT_ADDRESS = 0x02000000, 0x10000000, 0xC0001000
BRANCH_AND_LINK = 0x48000001
TEXT_SIZE = 0x100
DATA = b"data kept uncompressed"
# Section indices of the synthetic RPX.
TEXT, DATA_SECTION, BSS, IMPORTS, SYMBOLS, RELOCATIONS = 1, 2, 3, 4, 5, 6
# Symbols: 0 null, 1 the data section, 2 an imported function, 3 undefined.
DATA_SYMBOL, IMPORT_SYMBOL, UNDEFINED_SYMBOL = 1, 2, 3
CALL_SITE, UNDEFINED_CALL_SITE, LOAD_SITE = 0x10, 0x20, 0x30


def _text(load_address: int) -> bytes:
    """Code whose calls are unlinked and whose load of `load_address` is prelinked."""
    text = bytearray(TEXT_SIZE)
    struct.pack_into(">I", text, CALL_SITE, BRANCH_AND_LINK)
    struct.pack_into(">I", text, UNDEFINED_CALL_SITE, BRANCH_AND_LINK)
    high = ((load_address + 0x8000) >> 16) & 0xFFFF
    struct.pack_into(">IHH", text, LOAD_SITE, 0x3C600000 | high, 0x3863, load_address & 0xFFFF)
    return bytes(text)


def _rpx(
    *,
    text_size_claimed: int | None = None,
    file_type: int = RPL_FILE_TYPE,
    prelinked_load: int = DATA_ADDRESS + 4,
    load_kind: int = R_PPC_ADDR16_LO,
) -> bytes:
    text = _text(prelinked_load)
    compressed = struct.pack(">I", text_size_claimed or len(text)) + zlib.compress(text)
    symbols = b"".join(
        [
            _SYMBOL.pack(0, 0, 0, 0, 0, 0),
            _SYMBOL.pack(0, DATA_ADDRESS, 0, 3, 0, DATA_SECTION),
            _SYMBOL.pack(0, IMPORT_ADDRESS + 8, 0, 0x12, 0, IMPORTS),
            _SYMBOL.pack(0, 0, 0, 0x12, 0, 0),
        ]
    )
    relocations = b"".join(
        [
            _RELOCATION.pack(TEXT_ADDRESS + CALL_SITE, IMPORT_SYMBOL << 8 | R_PPC_REL24, 0),
            _RELOCATION.pack(
                TEXT_ADDRESS + UNDEFINED_CALL_SITE, UNDEFINED_SYMBOL << 8 | R_PPC_REL24, 0
            ),
            _RELOCATION.pack(TEXT_ADDRESS + LOAD_SITE + 2, DATA_SYMBOL << 8 | R_PPC_ADDR16_HA, 4),
            _RELOCATION.pack(TEXT_ADDRESS + LOAD_SITE + 6, DATA_SYMBOL << 8 | load_kind, 4),
        ]
    )
    stubs = bytes(16)
    body = bytearray(_HEADER.size)
    offsets = []
    for payload in (compressed, DATA, stubs, symbols, relocations):
        body += b"\0" * (-len(body) % 4)
        offsets.append(len(body))
        body += payload
    body += b"\0" * (-len(body) % 4)
    text_at, data_at, stubs_at, symbols_at, relocations_at = offsets
    sections = [
        (0, 0, 0, 0, 0, 0, 0, 0, 0, 0),
        (0, 1, 0x6 | SHF_RPL_ZLIB, TEXT_ADDRESS, text_at, len(compressed), 0, 0, 32, 0),
        (0, 1, 0x3, DATA_ADDRESS, data_at, len(DATA), 0, 0, 4, 0),
        (0, SHT_NOBITS, 0x3, DATA_ADDRESS + 0x1000, 0, 0x800, 0, 0, 64, 0),
        (0, SHT_RPL_IMPORTS, 0x6, IMPORT_ADDRESS, stubs_at, len(stubs), 0, 0, 4, 0),
        (0, SHT_SYMTAB, 0, 0, symbols_at, len(symbols), 0, 0, 4, 16),
        (0, SHT_RELA, 0, 0, relocations_at, len(relocations), SYMBOLS, TEXT, 4, 12),
    ]
    body[: _HEADER.size] = _HEADER.pack(
        b"\x7fELF\x01\x02\x01\xca\xfe" + b"\0" * 7,
        file_type,
        20,
        1,
        TEXT_ADDRESS,
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
    for section in sections:
        body += _SECTION.pack(*section)
    return bytes(body)


def _read(elf: bytes) -> tuple[tuple, list[tuple]]:
    header = _HEADER.unpack_from(elf)
    offset, count = header[6], header[12]
    return header, [_SECTION.unpack_from(elf, offset + index * 40) for index in range(count)]


def _contents(elf: bytes, section: tuple) -> bytes:
    return elf[section[4] : section[4] + section[5]]


def test_every_section_is_inflated_where_the_loader_places_it() -> None:
    elf, _ = to_elf(_rpx())
    header, sections = _read(elf)
    assert header[1] == 2, "an ordinary executable"
    assert header[0][7:9] == b"\0\0", "of the System V ABI"
    text, data, bss = sections[TEXT], sections[DATA_SECTION], sections[BSS]
    assert _contents(elf, text)[LOAD_SITE:] == _text(DATA_ADDRESS + 4)[LOAD_SITE:]
    assert text[2] & SHF_RPL_ZLIB == 0 and text[3] == TEXT_ADDRESS
    assert text[4] % 32 == 0, "at its alignment"
    assert _contents(elf, data) == DATA
    assert bss[5] == 0x800 and bss[1] == SHT_NOBITS, "bss keeps its size and holds no bytes"


def test_a_call_to_an_import_reaches_its_stub_placed_after_the_code() -> None:
    elf, report = to_elf(_rpx())
    _, sections = _read(elf)
    imports = sections[IMPORTS]
    assert imports[1] == 1 and imports[3] == TEXT_ADDRESS + TEXT_SIZE, "loaded after the code"
    (instruction,) = struct.unpack_from(">I", _contents(elf, sections[TEXT]), CALL_SITE)
    assert instruction == BRANCH_AND_LINK | (TEXT_SIZE + 8 - CALL_SITE)
    symbol = _SYMBOL.unpack_from(_contents(elf, sections[SYMBOLS]), IMPORT_SYMBOL * 16)
    assert symbol[1] == TEXT_ADDRESS + TEXT_SIZE + 8, "its symbol moves with it"
    assert (report.import_relocations, report.prelinked_relocations) == (1, 2)


def test_a_call_to_an_undefined_symbol_is_left_and_counted() -> None:
    elf, report = to_elf(_rpx())
    _, sections = _read(elf)
    (instruction,) = struct.unpack_from(">I", _contents(elf, sections[TEXT]), UNDEFINED_CALL_SITE)
    assert instruction == BRANCH_AND_LINK
    assert report.undefined_relocations == 1


def test_a_relocation_that_disagrees_with_the_prelinked_bytes_is_refused() -> None:
    with pytest.raises(LinkRefused, match="prelinked"):
        to_elf(_rpx(prelinked_load=DATA_ADDRESS + 8))


def test_a_relocation_type_it_does_not_know_is_refused() -> None:
    with pytest.raises(LinkRefused, match="type 11"):
        to_elf(_rpx(load_kind=11))


def test_a_file_that_is_not_an_rpx_is_refused() -> None:
    with pytest.raises(NotAnRpx, match="not an ELF"):
        to_elf(b"PK\x03\x04" + b"\0" * 60)
    with pytest.raises(NotAnRpx, match="file type"):
        to_elf(_rpx(file_type=2))


def test_a_section_that_inflates_to_another_size_than_it_names_is_refused() -> None:
    with pytest.raises(NotAnRpx, match="inflated"):
        to_elf(_rpx(text_size_claimed=TEXT_SIZE + 1))
