"""Link an inflated RPX's calls to its imports, so a disassembler follows them.

An RPX is prelinked at its section addresses except for its imports: each
`.fimport_*`/`.dimport_*` section holds the stubs of one library's functions
or data at 0xC0000000 and up, out of reach of a branch from `.text`, and the
loader relocates every reference to them at run time. Placing the import
sections right after `.text` and applying every relocation gives each call a
real target named by its import symbol. Every relocation against anything
else must reproduce the bytes already there; one that does not means the file
is not prelinked where it says, and is refused. A relocation against an
undefined symbol names no target (the loader sends it to address 0, a weak
reference never taken) and is left as it stands, counted.
"""

from __future__ import annotations

import struct
from collections.abc import Sequence
from dataclasses import dataclass
from typing import Protocol

SHN_UNDEF = 0
SHT_PROGBITS = 1
SHT_SYMTAB = 2
SHT_RELA = 4
SHT_RPL_IMPORTS = 0x80000002
SHF_EXECINSTR = 0x4
R_PPC_ADDR32 = 1
R_PPC_ADDR16_LO = 4
R_PPC_ADDR16_HI = 5
R_PPC_ADDR16_HA = 6
R_PPC_REL24 = 10
_SYMBOL = struct.Struct(">IIIBBH")
_RELOCATION = struct.Struct(">IIi")
_BRANCH_REACH = 1 << 25


class LinkRefused(ValueError):
    """The RPX cannot be linked as it stands; the message says why."""


class LinkedSection(Protocol):
    type: int
    flags: int
    addr: int
    size: int
    link: int
    info: int
    addralign: int


@dataclass(frozen=True)
class LinkReport:
    imports_placed_at: int
    import_relocations: int
    prelinked_relocations: int
    undefined_relocations: int


def link(sections: Sequence[LinkedSection], data: Sequence[bytearray]) -> LinkReport:
    """Place the import sections after the code and apply every relocation, in place."""
    moved = _place_imports(sections)
    for section, table in zip(sections, data, strict=True):
        if section.type == SHT_SYMTAB:
            _shift_symbols(table, moved)
    import_relocations = prelinked = undefined = 0
    for section, relocations in zip(sections, data, strict=True):
        if section.type != SHT_RELA:
            continue
        symbols, target = data[section.link], sections[section.info]
        for site, info, addend in _RELOCATION.iter_unpack(relocations):
            _, value, _, _, _, shndx = _SYMBOL.unpack_from(symbols, (info >> 8) * _SYMBOL.size)
            if shndx == SHN_UNDEF:
                undefined += 1
                continue
            changed = _apply(
                data[section.info], site - target.addr, site, info & 0xFF, value + addend
            )
            if shndx in moved:
                import_relocations += 1
            elif changed:
                raise LinkRefused(f"relocation at {site:#x} changes bytes it names prelinked")
            else:
                prelinked += 1
    placed = min((sections[index].addr for index in moved), default=0)
    return LinkReport(placed, import_relocations, prelinked, undefined)


def _place_imports(sections: Sequence[LinkedSection]) -> dict[int, int]:
    """Move each import section after the code; the address shift by section index."""
    cursor = max(
        (
            s.addr + s.size
            for s in sections
            if s.flags & SHF_EXECINSTR and s.type != SHT_RPL_IMPORTS
        ),
        default=0,
    )
    moved: dict[int, int] = {}
    for index, section in enumerate(sections):
        if section.type != SHT_RPL_IMPORTS:
            continue
        cursor += -cursor % max(section.addralign, 1)
        moved[index] = cursor - section.addr
        section.addr = cursor
        section.type = SHT_PROGBITS
        cursor += section.size
    return moved


def _shift_symbols(table: bytearray, moved: dict[int, int]) -> None:
    for offset in range(0, len(table), _SYMBOL.size):
        name, value, size, info, other, shndx = _SYMBOL.unpack_from(table, offset)
        if shndx in moved:
            _SYMBOL.pack_into(table, offset, name, value + moved[shndx], size, info, other, shndx)


def _apply(data: bytearray, at: int, site: int, kind: int, value: int) -> bool:
    """Write one relocation's field; whether that changed the bytes."""
    width = 2 if kind in _HALF_KINDS else 4
    if at < 0 or at + width > len(data):
        raise LinkRefused(f"relocation at {site:#x} lies outside the section it names")
    if kind == R_PPC_ADDR32:
        return _store(data, at, ">I", value & 0xFFFFFFFF)
    if kind in _HALF_KINDS:
        return _store(data, at, ">H", _HALF_KINDS[kind](value))
    if kind == R_PPC_REL24:
        displacement = value - site
        if not -_BRANCH_REACH <= displacement < _BRANCH_REACH:
            raise LinkRefused(f"branch at {site:#x} cannot reach {value:#x}")
        (instruction,) = struct.unpack_from(">I", data, at)
        return _store(data, at, ">I", (instruction & 0xFC000003) | (displacement & 0x03FFFFFC))
    raise LinkRefused(f"relocation type {kind} at {site:#x} is not one this linker applies")


_HALF_KINDS = {
    R_PPC_ADDR16_LO: lambda value: value & 0xFFFF,
    R_PPC_ADDR16_HI: lambda value: (value >> 16) & 0xFFFF,
    R_PPC_ADDR16_HA: lambda value: ((value + 0x8000) >> 16) & 0xFFFF,
}


def _store(data: bytearray, at: int, layout: str, value: int) -> bool:
    before = bytes(data[at : at + struct.calcsize(layout)])
    struct.pack_into(layout, data, at, value)
    return bytes(data[at : at + struct.calcsize(layout)]) != before
