#!/usr/bin/env python3
"""Fail if any x64 exe/dll under a directory hard-imports an API Windows 7 lacks.

A hard import the loader cannot resolve stops the process before main(), so one
Windows 10 DLL in the Qt5 artifact is enough to break it for every Windows 7 user,
and nothing in CI runs on Windows 7 to notice. This is a tripwire, not a proof:
it knows the offenders seen in practice (see POST_WIN7_FUNCTIONS) and flags any
API-set DLL other than the UCRT's, which Windows 7 only has stubs for.
"""

import struct
import sys
from pathlib import Path

POST_WIN7_FUNCTIONS = {
    "SetThreadDescription",
    "GetThreadDescription",
    "WaitOnAddress",
    "WakeByAddressSingle",
    "WakeByAddressAll",
    "GetSystemTimePreciseAsFileTime",
    "CreateFile2",
    "GetCurrentPackageId",
    "GetCurrentPackageFullName",
    "SetProcessMitigationPolicy",
    "GetProcessMitigationPolicy",
    "PrefetchVirtualMemory",
    "VirtualAlloc2",
    "MapViewOfFile3",
    "SetThreadInformation",
    "GetThreadInformation",
    "SetProcessInformation",
    "GetOverlappedResultEx",
    "CompareObjectHandles",
    "GetDpiForWindow",
    "GetDpiForSystem",
    "SetThreadDpiAwarenessContext",
    "SetProcessDpiAwarenessContext",
    "ProcessPrng",
}

PE32_PLUS = 0x20B


def imports(path):
    data = path.read_bytes()
    if data[:2] != b"MZ":
        return {}
    pe = struct.unpack_from("<I", data, 0x3C)[0]
    if data[pe:pe + 4] != b"PE\0\0":
        return {}
    section_count = struct.unpack_from("<H", data, pe + 6)[0]
    optional_size = struct.unpack_from("<H", data, pe + 20)[0]
    optional = pe + 24
    if struct.unpack_from("<H", data, optional)[0] != PE32_PLUS:
        return {}
    import_rva = struct.unpack_from("<I", data, optional + 112 + 8)[0]
    if not import_rva:
        return {}

    sections = []
    for i in range(section_count):
        header = optional + optional_size + i * 40
        virtual_size, virtual_address, raw_size, raw_offset = struct.unpack_from("<IIII", data, header + 8)
        sections.append((virtual_address, max(virtual_size, raw_size), raw_offset))

    def offset(rva):
        for virtual_address, size, raw_offset in sections:
            if virtual_address <= rva < virtual_address + size:
                return rva - virtual_address + raw_offset
        raise ValueError(f"rva {rva:#x} is outside every section")

    def cstring(at):
        return data[at:data.index(b"\0", at)].decode("latin1")

    result = {}
    descriptor = offset(import_rva)
    while True:
        lookup_rva, _, _, name_rva, address_rva = struct.unpack_from("<IIIII", data, descriptor)
        if not name_rva:
            return result
        functions = result.setdefault(cstring(offset(name_rva)), [])
        thunk = offset(lookup_rva or address_rva)
        while True:
            entry = struct.unpack_from("<Q", data, thunk)[0]
            if not entry:
                break
            by_ordinal = entry >> 63
            if not by_ordinal:
                functions.append(cstring(offset(entry & 0x7FFFFFFF) + 2))
            thunk += 8
        descriptor += 20


def violations(path):
    for dll, functions in imports(path).items():
        name = dll.lower()
        if name.startswith(("api-ms-win-", "ext-ms-win-")) and not name.startswith("api-ms-win-crt-"):
            yield f"{path.name}: imports {', '.join(functions)} from {dll}"
            continue
        for function in functions:
            if function in POST_WIN7_FUNCTIONS:
                yield f"{path.name}: imports {function} from {dll}"


def main():
    if len(sys.argv) != 2:
        sys.exit(f"usage: {sys.argv[0]} <artifact directory>")

    root = Path(sys.argv[1])
    binaries = sorted(p for p in root.rglob("*") if p.suffix.lower() in (".exe", ".dll"))
    if not binaries:
        sys.exit(f"no exe or dll found under {root}")

    found = [line for binary in binaries for line in violations(binary)]
    for line in found:
        print(line)
    if found:
        sys.exit(f"{len(found)} import(s) that Windows 7 cannot resolve")
    print(f"{len(binaries)} binaries checked, none import a post-Windows 7 API")


if __name__ == "__main__":
    main()
