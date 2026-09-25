"""A Wii U executable (RPX/RPL) as a plain ELF a disassembler loads.

An RPX is a big-endian 32-bit PowerPC ELF with no program headers, whose
section data may each be zlib-compressed (flag SHF_RPL_ZLIB: a big-endian
uncompressed size, then the stream), and whose file type (0xFE01) and ABI
(0xCA) no stock ELF loader knows. Decompressing every section and naming the
file an ordinary executable leaves every section at the address the loader
places it, so addresses read from a running title are addresses in the ELF.
"""

from __future__ import annotations

import struct
import zlib
from dataclasses import dataclass

ELF_MAGIC = b"\x7fELF"
RPL_FILE_TYPE = 0xFE01
EXECUTABLE_FILE_TYPE = 2
SHF_RPL_ZLIB = 0x08000000
SHT_NOBITS = 8
_HEADER = struct.Struct(">16sHHIIIIIHHHHHH")
_SECTION = struct.Struct(">IIIIIIIIII")


class NotAnRpx(ValueError):
    """The bytes are not an RPX this converter reads; the message says why."""


@dataclass
class Section:
    name: int
    type: int
    flags: int
    addr: int
    offset: int
    size: int
    link: int
    info: int
    addralign: int
    entsize: int


def _sections(image: bytes, offset: int, count: int, entry_size: int) -> list[Section]:
    if entry_size != _SECTION.size:
        raise NotAnRpx(f"section headers are {entry_size} bytes, not {_SECTION.size}")
    end = offset + count * entry_size
    if end > len(image):
        raise NotAnRpx(f"section headers run to {end:#x}, past the file's end {len(image):#x}")
    return [
        Section(*_SECTION.unpack_from(image, offset + index * entry_size)) for index in range(count)
    ]


def _data(image: bytes, section: Section) -> bytes:
    if section.type == SHT_NOBITS or section.size == 0:
        return b""
    raw = image[section.offset : section.offset + section.size]
    if len(raw) != section.size:
        raise NotAnRpx(f"a section at {section.offset:#x} runs past the file's end")
    if not section.flags & SHF_RPL_ZLIB:
        return raw
    (inflated_size,) = struct.unpack_from(">I", raw)
    data = zlib.decompress(raw[4:])
    if len(data) != inflated_size:
        raise NotAnRpx(f"a section inflated to {len(data)} bytes, not the {inflated_size} it names")
    return data


def to_elf(image: bytes) -> bytes:
    """The RPX `image` as an executable ELF with every section decompressed."""
    if image[:4] != ELF_MAGIC or len(image) < _HEADER.size:
        raise NotAnRpx("not an ELF file")
    header = list(_HEADER.unpack_from(image))
    ident, file_type = header[0], header[1]
    if ident[4] != 1 or ident[5] != 2:
        raise NotAnRpx("not a 32-bit big-endian ELF")
    if file_type != RPL_FILE_TYPE:
        raise NotAnRpx(f"file type {file_type:#x} is not an RPX's {RPL_FILE_TYPE:#x}")
    section_offset, section_entry_size, section_count = header[6], header[11], header[12]
    sections = _sections(image, section_offset, section_count, section_entry_size)
    body = bytearray(_HEADER.size)
    for section in sections:
        data = _data(image, section)
        section.flags &= ~SHF_RPL_ZLIB
        if section.type == SHT_NOBITS:
            section.offset = len(body)
            continue
        alignment = max(section.addralign, 1)
        body.extend(b"\0" * (-len(body) % alignment))
        section.offset = len(body) if data else 0
        section.size = len(data)
        body.extend(data)
    body.extend(b"\0" * (-len(body) % 4))
    header[0] = ident[:7] + b"\0\0" + ident[9:]  # the System V ABI, version 0
    header[1] = EXECUTABLE_FILE_TYPE
    header[6] = len(body)
    body[: _HEADER.size] = _HEADER.pack(*header)
    for section in sections:
        body.extend(
            _SECTION.pack(
                section.name,
                section.type,
                section.flags,
                section.addr,
                section.offset,
                section.size,
                section.link,
                section.info,
                section.addralign,
                section.entsize,
            )
        )
    return bytes(body)
