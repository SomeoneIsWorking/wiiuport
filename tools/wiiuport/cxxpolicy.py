"""Three ownership rules clang-tidy cannot express, checked on the real AST.

A text search would count comments, strings and dead references alongside live
declarations, so every rule here runs over libclang cursors instead:

1. project-owned functions and variables in the global namespace;
2. ``extern`` declarations of project-owned functions and variables;
3. block-scope ``static``, ``const`` and ``constexpr`` variables.

An ``extern "C"`` boundary is exempt from rules 1 and 2 -- a required C ABI
declaration belongs in that boundary's header and has nowhere else to live.
``main`` is exempt from rule 1 for the same reason.

Rule 3 tests the variable's own type, not its pointee, so ``const T*`` and
``const T&`` locals and const-correct parameters stay legal.
"""

from __future__ import annotations

import json
import subprocess
from dataclasses import dataclass
from functools import cache
from pathlib import Path

from clang.cindex import (
    Config,
    Cursor,
    CursorKind,
    Index,
    StorageClass,
    TranslationUnit,
    TranslationUnitLoadError,
)

from .hostdeps import MissingHostPackages, Requirement, check
from .paths import Layout
from .structure import FIRST_PARTY_CXX_ROOTS

BASELINE_ARGUMENTS: tuple[str, ...] = ("-std=c++20", "-xc++")
"""Flags for a file the compile database does not name, such as a header.

A header is never a translation unit, so no database can name one, and its own
includes are written against the library's include root. Without that root on
the command line every first-party header fails to parse, which this gate
correctly reports as a refusal rather than a pass.
"""

INCLUDE_ROOTS: tuple[str, ...] = ("src", "external/cemu/src", "external/lucent/include")
"""What the library compiles against, matching the include directories
``src/wiiuport/CMakeLists.txt`` exposes: its own tree, the fork's, whose hook
interface it implements rather than copies, and lucent's. Used only for a file
the compile database cannot name, which is every header."""

_FUNCTION_SCOPES = frozenset(
    {
        CursorKind.FUNCTION_DECL,
        CursorKind.CXX_METHOD,
        CursorKind.CONSTRUCTOR,
        CursorKind.DESTRUCTOR,
        CursorKind.FUNCTION_TEMPLATE,
        CursorKind.CONVERSION_FUNCTION,
    }
)

_DECLARATION_KINDS = frozenset({CursorKind.FUNCTION_DECL, CursorKind.VAR_DECL})

_ABI_EXEMPT_NAMES = frozenset({"main"})


class CxxPolicyUnavailable(RuntimeError):
    """The corpus cannot be parsed, so silence would not mean compliance."""


@dataclass(frozen=True)
class Finding:
    """One violation, located where a reader can open it."""

    file: str
    line: int
    rule: str
    detail: str

    def __str__(self) -> str:
        return f"{self.file}:{self.line}: {self.rule}: {self.detail}"


@dataclass(frozen=True)
class PolicyReport:
    """Findings alongside the denominator, so a clean run is distinguishable
    from a run that never looked at anything."""

    scanned: tuple[str, ...]
    findings: tuple[Finding, ...]

    @property
    def summary(self) -> str:
        return f"parsed {len(self.scanned)} first-party translation units, {len(self.findings)} findings"


def _inside_c_linkage(cursor: Cursor) -> bool:
    parent = cursor.lexical_parent
    while parent is not None:
        if parent.kind == CursorKind.LINKAGE_SPEC:
            return True
        parent = parent.lexical_parent
    return False


def _check_cursor(cursor: Cursor, relative: str) -> list[Finding]:
    if cursor.kind not in _DECLARATION_KINDS:
        return []
    semantic = cursor.semantic_parent
    line = cursor.location.line
    findings: list[Finding] = []

    at_global_scope = semantic is not None and semantic.kind == CursorKind.TRANSLATION_UNIT
    if (
        at_global_scope
        and not _inside_c_linkage(cursor)
        and cursor.spelling not in _ABI_EXEMPT_NAMES
    ):
        findings.append(
            Finding(
                relative,
                line,
                "global-namespace API",
                f"'{cursor.spelling}' is declared in the global namespace; give it a "
                "project or subsystem namespace, or a class that owns its state",
            )
        )

    if cursor.storage_class == StorageClass.EXTERN and not _inside_c_linkage(cursor):
        findings.append(
            Finding(
                relative,
                line,
                "extern declaration",
                f"'{cursor.spelling}' is declared extern; include the owning header "
                "instead, and never use an extern global variable as shared state",
            )
        )

    if (
        cursor.kind == CursorKind.VAR_DECL
        and semantic is not None
        and semantic.kind in _FUNCTION_SCOPES
        and (cursor.storage_class == StorageClass.STATIC or cursor.type.is_const_qualified())
    ):
        kind = "static" if cursor.storage_class == StorageClass.STATIC else "const or constexpr"
        findings.append(
            Finding(
                relative,
                line,
                "block-scope declaration",
                f"'{cursor.spelling}' is a block-scope {kind} variable; put named "
                "constants in the owning header as inline constexpr members and keep "
                "computed values as ordinary locals",
            )
        )
    return findings


def _walk(cursor: Cursor, source: Path, relative: str) -> list[Finding]:
    findings: list[Finding] = []
    for child in cursor.walk_preorder():
        location = child.location.file
        if location is None or Path(location.name).resolve() != source:
            continue
        findings.extend(_check_cursor(child, relative))
    return findings


@cache
def _resource_directory() -> str:
    """Where the compiler keeps the builtin headers every parse needs.

    The libclang this gate loads is installed from PyPI and ships no headers of
    its own, so without this it cannot find ``stddef.h`` and every translation
    unit that reaches libc fails to parse. Asking the compiler that produced the
    compile database keeps one answer rather than a guessed path per clang
    release.
    """
    compiler = Requirement("C++ compiler (clang)", ("clang",), ("clang",), executables=("clang++",))
    try:
        check((compiler,))
    except MissingHostPackages as missing:
        raise CxxPolicyUnavailable(
            "the builtin header directory needed to parse first-party C++ is "
            f"unknown, so nothing was inspected.\n{missing}"
        ) from missing
    found = subprocess.run(
        ["clang++", "-print-resource-dir"], capture_output=True, text=True, check=False
    )
    if found.returncode != 0 or not found.stdout.strip():
        raise CxxPolicyUnavailable(
            "clang++ did not report a resource directory, so no translation unit "
            f"could be parsed: {(found.stdout + found.stderr).strip()}"
        )
    return found.stdout.strip()


@cache
def _load_matching_libclang() -> str:
    """Point the bindings at the libclang that matches the builtin headers.

    The PyPI bindings bundle their own, older, library. Parsing clang's own
    builtin headers with it fails on intrinsics and standard library internals
    that the newer compiler introduced, and the gate then reports a parse
    error instead of inspecting anything. One clang answers for both.
    """
    resource = Path(_resource_directory())
    major = resource.name
    candidates = [
        *Path("/usr/lib64").glob(f"libclang.so.{major}*"),
        *Path("/usr/lib").glob(f"libclang.so.{major}*"),
        *Path("/usr/lib/llvm-" + major + "/lib").glob("libclang.so*"),
        *(resource.parent.parent / "lib").glob("libclang.so*"),
    ]
    for candidate in candidates:
        if candidate.is_file():
            Config.set_library_file(str(candidate))
            return str(candidate)
    raise CxxPolicyUnavailable(
        f"no libclang for clang {major} was found beside the compiler, so no "
        "translation unit could be parsed. Install the matching library "
        f"(Fedora: sudo dnf install clang-libs; Debian/Ubuntu: sudo apt install libclang-{major}-dev)."
    )


def _compile_arguments(source: Path, database: dict[str, list[str]], root: Path) -> list[str]:
    resource = [f"-resource-dir={_resource_directory()}"]
    arguments = database.get(str(source))
    if arguments is None and source.suffix in {".h", ".hpp"}:
        # A header is read the way its own implementation reads it, which is
        # what puts the fork's precompiled declarations in scope. Parsing it
        # bare reports hundreds of unknown types and inspects nothing.
        arguments = database.get(str(source.with_suffix(".cpp")))
        if arguments is not None:
            # Those flags name a C++ translation unit; libclang would read a
            # .h as C and reject them.
            return ["-xc++-header", *arguments, *resource]
    if arguments is not None:
        return [*arguments, *resource]
    includes = [f"-I{root / include}" for include in INCLUDE_ROOTS]
    return [*BASELINE_ARGUMENTS, *includes, *resource]


def _load_compile_database(path: Path) -> dict[str, list[str]]:
    """Map each named translation unit to its own flags, minus the output and
    input operands libclang supplies itself."""
    entries = json.loads(path.read_text(encoding="utf-8"))
    database: dict[str, list[str]] = {}
    for entry in entries:
        directory = Path(entry["directory"])
        file = (directory / entry["file"]).resolve()
        arguments = entry.get("arguments") or entry["command"].split()
        kept: list[str] = []
        skip = 0
        for index, argument in enumerate(arguments[1:]):
            if skip:
                skip -= 1
                continue
            if argument in {"-o", "-c"}:
                skip = 1 if argument == "-o" else 0
                continue
            # A precompiled header is built by the compiler that will consume
            # it; libclang is a different build and rejects it. The textual
            # `-include` of the same header stays, so the declarations it
            # brings are still in scope.
            if argument == "-Xclang" and arguments[index + 2 : index + 3] == ["-include-pch"]:
                skip = 3
                continue
            if Path(argument).resolve() == file:
                continue
            kept.append(argument)
        database[str(file)] = kept
    return database


def analyse(sources: list[Path], root: Path, database: dict[str, list[str]]) -> PolicyReport:
    """Parse each source and return its findings with the parsed denominator."""
    _load_matching_libclang()
    index = Index.create()
    scanned: list[str] = []
    findings: list[Finding] = []
    for source in sources:
        resolved = source.resolve()
        relative = resolved.relative_to(root).as_posix()
        try:
            unit = index.parse(
                str(resolved),
                args=_compile_arguments(resolved, database, root),
                options=TranslationUnit.PARSE_DETAILED_PROCESSING_RECORD,
            )
        except TranslationUnitLoadError as failure:
            raise CxxPolicyUnavailable(
                f"{relative} could not be parsed at all, so its declarations were "
                f"never inspected: {failure}"
            ) from failure
        fatal = [str(d) for d in unit.diagnostics if d.severity >= 4]
        if fatal:
            raise CxxPolicyUnavailable(
                f"{relative} could not be parsed, so its declarations were never "
                f"inspected: {fatal[0]}"
            )
        scanned.append(relative)
        findings.extend(_walk(unit.cursor, resolved, relative))
    return PolicyReport(tuple(scanned), tuple(findings))


def first_party_sources(root: Path) -> list[Path]:
    sources: list[Path] = []
    for relative in FIRST_PARTY_CXX_ROOTS:
        directory = root / relative
        if not directory.is_dir():
            continue
        for suffix in ("*.cpp", "*.h", "*.hpp"):
            sources.extend(sorted(directory.rglob(suffix)))
    return sources


def check_cxx_policy(layout: Layout) -> PolicyReport:
    """Check the shipping source tree, refusing when it exists but cannot be read.

    An empty ``src`` tree reports zero of zero rather than passing quietly: the
    caller prints the denominator either way.
    """
    root = layout.root.resolve()
    sources = first_party_sources(root)
    database: dict[str, list[str]] = {}
    # Two builds compile first-party sources: the standalone library and the
    # fork, which is the only one that compiles the shell. Both databases are
    # read, so a file is analysed with the flags it is actually built with.
    for build in (layout.wiiuport_build, layout.cemu_build):
        compile_commands = build / "compile_commands.json"
        if sources and compile_commands.is_file():
            database.update(_load_compile_database(compile_commands))
    return analyse(sources, root, database)
