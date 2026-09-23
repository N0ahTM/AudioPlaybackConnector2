#!/usr/bin/env python3
"""Report potentially blocking or external call chains made while a C++ owner lock is held.

This is an investigation aid, not a pass/fail gate. It consumes the compile databases
written by MSBuild's ClangTidy target and emits source locations for manual triage.
The bounded call graph excludes header-only definitions and implicit destructors;
native SRW locks need a separate source review.
"""

from __future__ import annotations

import argparse
import ctypes as C
import json
import os
import re
from pathlib import Path


class Cursor(C.Structure):
    _fields_ = [("kind", C.c_uint), ("xdata", C.c_int), ("data", C.c_void_p * 3)]


class Type(C.Structure):
    _fields_ = [("kind", C.c_uint), ("data", C.c_void_p * 2)]


class String(C.Structure):
    _fields_ = [("data", C.c_void_p), ("private_flags", C.c_uint)]


class Location(C.Structure):
    _fields_ = [("ptr_data", C.c_void_p * 2), ("int_data", C.c_uint)]


class Range(C.Structure):
    _fields_ = [("ptr_data", C.c_void_p * 2), ("begin_int_data", C.c_uint), ("end_int_data", C.c_uint)]


VISITOR = C.CFUNCTYPE(C.c_uint, Cursor, Cursor, C.c_void_p)
LOCK_TYPE = re.compile(r"\b(?:lock_guard|scoped_lock|unique_lock)\s*<")
SINK = re.compile(
    r"(?:Wait|Sleep|Join|Invoke|Callback|Publish|Dispatch|Post|Send|Log|Trace|"
    r"Close|Connect|Disconnect|Threadpool|Timer|Cancel|Notify|Show|Launch|CacheNow|Handle|Destroy|"
    r"request_stop|erase|clear|reset|operator\(\))",
    re.IGNORECASE,
)


def bind(lib, name, result, *args):
    fn = getattr(lib, name)
    fn.restype = result
    fn.argtypes = args
    return fn


class Clang:
    def __init__(self, library: Path):
        self.lib = C.CDLL(str(library))
        self.create_index = bind(self.lib, "clang_createIndex", C.c_void_p, C.c_int, C.c_int)
        self.dispose_index = bind(self.lib, "clang_disposeIndex", None, C.c_void_p)
        self.parse = bind(
            self.lib, "clang_parseTranslationUnit", C.c_void_p, C.c_void_p, C.c_char_p,
            C.POINTER(C.c_char_p), C.c_int, C.c_void_p, C.c_uint, C.c_uint,
        )
        self.dispose_tu = bind(self.lib, "clang_disposeTranslationUnit", None, C.c_void_p)
        self.tu_cursor = bind(self.lib, "clang_getTranslationUnitCursor", Cursor, C.c_void_p)
        self.num_diagnostics = bind(self.lib, "clang_getNumDiagnostics", C.c_uint, C.c_void_p)
        self.diagnostic = bind(self.lib, "clang_getDiagnostic", C.c_void_p, C.c_void_p, C.c_uint)
        self.format_diagnostic = bind(self.lib, "clang_formatDiagnostic", String, C.c_void_p, C.c_uint)
        self.diagnostic_severity = bind(self.lib, "clang_getDiagnosticSeverity", C.c_uint, C.c_void_p)
        self.dispose_diagnostic = bind(self.lib, "clang_disposeDiagnostic", None, C.c_void_p)
        self.visit = bind(self.lib, "clang_visitChildren", C.c_uint, Cursor, VISITOR, C.c_void_p)
        self.kind_name = bind(self.lib, "clang_getCursorKindSpelling", String, C.c_uint)
        self.cursor_name = bind(self.lib, "clang_getCursorSpelling", String, Cursor)
        self.usr = bind(self.lib, "clang_getCursorUSR", String, Cursor)
        self.referenced = bind(self.lib, "clang_getCursorReferenced", Cursor, Cursor)
        self.cursor_type = bind(self.lib, "clang_getCursorType", Type, Cursor)
        self.type_name = bind(self.lib, "clang_getTypeSpelling", String, Type)
        self.extent = bind(self.lib, "clang_getCursorExtent", Range, Cursor)
        self.start = bind(self.lib, "clang_getRangeStart", Location, Range)
        self.end = bind(self.lib, "clang_getRangeEnd", Location, Range)
        self.locate = bind(
            self.lib, "clang_getSpellingLocation", None, Location, C.POINTER(C.c_void_p),
            C.POINTER(C.c_uint), C.POINTER(C.c_uint), C.POINTER(C.c_uint),
        )
        self.file_name = bind(self.lib, "clang_getFileName", String, C.c_void_p)
        self.is_main = bind(self.lib, "clang_Location_isFromMainFile", C.c_int, Location)
        self.is_definition = bind(self.lib, "clang_isCursorDefinition", C.c_uint, Cursor)
        self.c_string = bind(self.lib, "clang_getCString", C.c_char_p, String)
        self.dispose_string = bind(self.lib, "clang_disposeString", None, String)

    def text(self, value: String) -> str:
        try:
            return (self.c_string(value) or b"").decode("utf-8", errors="replace")
        finally:
            self.dispose_string(value)

    def position(self, loc: Location) -> tuple[str, int, int]:
        file = C.c_void_p()
        line = C.c_uint()
        col = C.c_uint()
        offset = C.c_uint()
        self.locate(loc, C.byref(file), C.byref(line), C.byref(col), C.byref(offset))
        return self.text(self.file_name(file)), line.value, offset.value

    def span(self, cursor: Cursor) -> tuple[int, int, int]:
        extent = self.extent(cursor)
        _, line, start = self.position(self.start(extent))
        _, _, end = self.position(self.end(extent))
        return line, start, end

    def children(self, cursor: Cursor, recursive: bool = False, main_only: bool = True,
                 skip_lambdas: bool = False) -> list[Cursor]:
        found = []

        @VISITOR
        def collect(child, _parent, _data):
            if not main_only or self.is_main(self.start(self.extent(child))):
                if skip_lambdas and self.text(self.kind_name(child.kind)) == "LambdaExpr":
                    return 1
                found.append(child)
                return 2 if recursive else 1
            return 1

        self.visit(cursor, collect, None)
        return found


def windows_argv(command: str) -> list[str]:
    shell = C.windll.shell32
    shell.CommandLineToArgvW.restype = C.POINTER(C.c_wchar_p)
    shell.CommandLineToArgvW.argtypes = [C.c_wchar_p, C.POINTER(C.c_int)]
    local_free = C.windll.kernel32.LocalFree
    local_free.argtypes = [C.c_void_p]
    local_free.restype = C.c_void_p
    count = C.c_int()
    result = shell.CommandLineToArgvW(command, C.byref(count))
    if not result:
        raise OSError("CommandLineToArgvW failed")
    try:
        return [result[i] for i in range(count.value)]
    finally:
        local_free(C.cast(result, C.c_void_p))


def calls_and_locks(clang: Clang, cursor: Cursor, source: bytes):
    nodes = clang.children(cursor, recursive=True, skip_lambdas=True)
    blocks = []
    locks = []
    calls = []
    for node in nodes:
        kind = clang.text(clang.kind_name(node.kind))
        line, start, end = clang.span(node)
        if kind == "CompoundStmt":
            blocks.append((start, end))
        elif kind == "VarDecl" and LOCK_TYPE.search(clang.text(clang.type_name(clang.cursor_type(node)))):
            locks.append((clang.text(clang.cursor_name(node)), line, start))
        elif kind in ("CallExpr", "CXXMemberCallExpr", "CXXOperatorCallExpr"):
            target = clang.referenced(node)
            name = clang.text(clang.cursor_name(node))
            if not name:
                name = source[start:end].decode("utf-8", errors="replace").split("(", 1)[0].strip()[:120]
            if clang.text(clang.kind_name(target.kind)) in ("ParmDecl", "VarDecl", "FieldDecl"):
                name = f"callback:{name}"
            calls.append((name, clang.text(clang.usr(target)), line, start, end))
    guarded = []
    for lock_name, line, offset in locks:
        enclosing = [(begin, end) for begin, end in blocks if begin <= offset < end]
        if not enclosing:
            continue
        _, scope_end = min(enclosing, key=lambda block: block[1] - block[0])
        held = True
        for name, target, call_line, call_start, call_end in sorted(calls, key=lambda call: call[3]):
            if not offset < call_start < scope_end:
                continue
            call_text = source[call_start:call_end].decode("utf-8", errors="replace")
            if name == "unlock" and re.search(rf"\b{re.escape(lock_name)}\s*\.\s*unlock\s*\(", call_text):
                held = False
            elif name == "lock" and re.search(rf"\b{re.escape(lock_name)}\s*\.\s*lock\s*\(", call_text):
                held = True
            elif held:
                guarded.append((lock_name, line, name, target, call_line))
    return calls, guarded


def main() -> int:
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("compile_databases", nargs="+", type=Path)
    parser.add_argument("--libclang", type=Path, default=Path(r"C:\Program Files\LLVM\bin\libclang.dll"))
    parser.add_argument("--resource-dir", default=r"C:\Program Files\LLVM\lib\clang\22")
    parser.add_argument("--output", type=Path, required=True)
    parser.add_argument("--debug", action="store_true")
    parser.add_argument("--file", action="append", help="Process only a translation unit with this basename")
    args = parser.parse_args()

    clang = Clang(args.libclang)
    definitions = {}
    guarded_calls = []
    files = 0
    errors = []
    diagnostic_errors = []
    index = clang.create_index(0, 0)
    cwd = Path.cwd()
    try:
        for database in args.compile_databases:
            for entry in json.loads(database.read_text(encoding="utf-8")):
                filename = Path(entry["file"])
                if args.file and filename.name not in args.file:
                    continue
                command = windows_argv(entry["command"])[1:]
                command = [arg for arg in command if arg not in (str(filename), "/WX")]
                command += ["--driver-mode=cl", f"-resource-dir={args.resource_dir}",
                            "-Wno-unused-command-line-argument", "-Wno-missing-field-initializers"]
                encoded = [arg.encode("utf-8") for arg in command]
                argv = (C.c_char_p * len(encoded))(*encoded)
                os.chdir(entry["directory"])
                tu = clang.parse(index, str(filename).encode("utf-8"), argv, len(encoded), None, 0, 0)
                os.chdir(cwd)
                if not tu:
                    errors.append(str(filename))
                    continue
                try:
                    files += 1
                    for number in range(clang.num_diagnostics(tu)):
                        diagnostic = clang.diagnostic(tu, number)
                        severity = clang.diagnostic_severity(diagnostic)
                        if severity >= 3:
                            diagnostic_errors.append((str(filename), clang.text(clang.format_diagnostic(diagnostic, 0))))
                        if severity >= 4:
                            errors.append(str(filename))
                        if args.debug and (number < 8 or severity >= 3):
                            print(clang.text(clang.format_diagnostic(diagnostic, 0)))
                        clang.dispose_diagnostic(diagnostic)
                    source = filename.read_bytes()
                    nodes = clang.children(clang.tu_cursor(tu), recursive=True)
                    if args.debug:
                        print("root", [(clang.text(clang.kind_name(node.kind)), clang.position(clang.start(clang.extent(node))),
                                        clang.is_main(clang.start(clang.extent(node))))
                                       for node in clang.children(clang.tu_cursor(tu), main_only=False)[-8:]])
                        print(filename.name, "nodes", len(nodes), "kinds",
                              [clang.text(clang.kind_name(node.kind)) for node in nodes[:8]])
                    for node in nodes:
                        kind = clang.text(clang.kind_name(node.kind))
                        if kind not in ("FunctionDecl", "CXXMethod", "Constructor", "Destructor", "FunctionTemplate"):
                            continue
                        if not clang.is_definition(node):
                            continue
                        usr = clang.text(clang.usr(node))
                        if not usr:
                            continue
                        name = clang.text(clang.cursor_name(node))
                        calls, guarded = calls_and_locks(clang, node, source)
                        definitions[usr] = (name, [(target, call_name) for call_name, target, _, _, _ in calls])
                        guarded_calls.extend((str(filename), name, lock, lock_line, call_name, target, call_line)
                                             for lock, lock_line, call_name, target, call_line in guarded)
                finally:
                    clang.dispose_tu(tu)
    finally:
        os.chdir(cwd)
        clang.dispose_index(index)

    def paths(target: str, name: str, seen: frozenset[str], depth: int):
        if depth == 0 or target in seen:
            return []
        if target not in definitions:
            return [[name]] if SINK.search(name) else []
        result = []
        for next_target, next_name in definitions[target][1]:
            for suffix in paths(next_target, next_name, seen | {target}, depth - 1):
                result.append([name, *suffix])
        return result

    findings = []
    for file, owner, lock, lock_line, name, target, call_line in guarded_calls:
        for path in paths(target, name, frozenset(), 6):
            findings.append({"file": file, "function": owner, "lock": lock, "lockLine": lock_line,
                             "callLine": call_line, "path": path})
    findings.sort(key=lambda item: (item["file"], item["callLine"], item["path"]))
    unresolved = sorted({(file, lock_line, call_line, name)
                         for file, _, _, lock_line, name, target, call_line in guarded_calls if not target})
    args.output.write_text(json.dumps({"translationUnits": files, "parseFailures": sorted(set(errors)),
                                       "diagnosticErrors": diagnostic_errors,
                                       "guardedCalls": len(guarded_calls),
                                       "unresolvedGuardedCalls": [dict(file=file, lockLine=lock_line,
                                                                        callLine=call_line, name=name)
                                                                  for file, lock_line, call_line, name in unresolved],
                                       "findings": findings}, indent=2),
                           encoding="utf-8")
    print(f"translationUnits={files} definitions={len(definitions)} parseFailures={len(set(errors))} "
          f"diagnosticErrors={len(diagnostic_errors)} guardedCalls={len(guarded_calls)} "
          f"unresolvedGuardedCalls={len(unresolved)} "
          f"candidatePaths={len(findings)} output={args.output}")
    return 1 if not files or errors or diagnostic_errors else 0


if __name__ == "__main__":
    raise SystemExit(main())
