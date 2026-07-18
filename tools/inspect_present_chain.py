"""Read-only inspection of the live IDXGISwapChain::Present detour chain."""

import ctypes
import ctypes.wintypes as wintypes
import re
import struct
from pathlib import Path


PROCESS_VM_READ = 0x0010
PROCESS_QUERY_INFORMATION = 0x0400
TH32CS_SNAPPROCESS = 0x00000002
TH32CS_SNAPMODULE = 0x00000008
TH32CS_SNAPMODULE32 = 0x00000010
LOG_PATH = Path(
    r"F:\SteamLibrary\steamapps\common\BorderlandsGOTYEnhanced\Binaries\Win64\BL1GOTYVR.log"
)
kernel32 = ctypes.WinDLL("kernel32", use_last_error=True)


class ProcessEntry(ctypes.Structure):
    _fields_ = [
        ("dwSize", wintypes.DWORD), ("cntUsage", wintypes.DWORD),
        ("th32ProcessID", wintypes.DWORD), ("th32DefaultHeapID", ctypes.c_size_t),
        ("th32ModuleID", wintypes.DWORD), ("cntThreads", wintypes.DWORD),
        ("th32ParentProcessID", wintypes.DWORD), ("pcPriClassBase", wintypes.LONG),
        ("dwFlags", wintypes.DWORD), ("szExeFile", ctypes.c_char * 260),
    ]


class ModuleEntry(ctypes.Structure):
    _fields_ = [
        ("dwSize", wintypes.DWORD), ("th32ModuleID", wintypes.DWORD),
        ("th32ProcessID", wintypes.DWORD), ("GlblcntUsage", wintypes.DWORD),
        ("ProccntUsage", wintypes.DWORD), ("modBaseAddr", ctypes.c_void_p),
        ("modBaseSize", wintypes.DWORD), ("hModule", wintypes.HMODULE),
        ("szModule", ctypes.c_char * 256), ("szExePath", ctypes.c_char * 260),
    ]


def find_pid():
    snapshot = kernel32.CreateToolhelp32Snapshot(TH32CS_SNAPPROCESS, 0)
    entry = ProcessEntry()
    entry.dwSize = ctypes.sizeof(entry)
    found = kernel32.Process32First(snapshot, ctypes.byref(entry))
    while found:
        if entry.szExeFile.decode(errors="replace").lower() == "borderlandsgoty.exe":
            kernel32.CloseHandle(snapshot)
            return entry.th32ProcessID
        found = kernel32.Process32Next(snapshot, ctypes.byref(entry))
    kernel32.CloseHandle(snapshot)
    return None


def modules(pid):
    result = []
    snapshot = kernel32.CreateToolhelp32Snapshot(
        TH32CS_SNAPMODULE | TH32CS_SNAPMODULE32, pid
    )
    entry = ModuleEntry()
    entry.dwSize = ctypes.sizeof(entry)
    found = kernel32.Module32First(snapshot, ctypes.byref(entry))
    while found:
        base = entry.modBaseAddr or 0
        result.append((base, base + entry.modBaseSize, entry.szModule.decode(errors="replace")))
        found = kernel32.Module32Next(snapshot, ctypes.byref(entry))
    kernel32.CloseHandle(snapshot)
    return result


def read(process, address, size):
    buffer = ctypes.create_string_buffer(size)
    count = ctypes.c_size_t()
    if not kernel32.ReadProcessMemory(
        process, ctypes.c_void_p(address), buffer, size, ctypes.byref(count)
    ) or count.value != size:
        return None
    return buffer.raw


def module_name(module_list, address):
    for start, end, name in module_list:
        if start <= address < end:
            return f"{name}+0x{address - start:X}"
    return "unmapped"


def next_jump(process, address, code):
    if code[0] == 0xE9:
        return address + 5 + struct.unpack_from("<i", code, 1)[0]
    if code[:2] == b"\xFF\x25":
        pointer = address + 6 + struct.unpack_from("<i", code, 2)[0]
        data = read(process, pointer, 8)
        return struct.unpack("<Q", data)[0] if data else None
    if code[:2] == b"\x48\xB8" and code[10:12] == b"\xFF\xE0":
        return struct.unpack_from("<Q", code, 2)[0]
    return None


def main():
    pid = find_pid()
    if not pid:
        raise SystemExit("BorderlandsGOTY.exe is not running")
    process = kernel32.OpenProcess(
        PROCESS_VM_READ | PROCESS_QUERY_INFORMATION, False, pid
    )
    log = LOG_PATH.read_text(encoding="utf-8", errors="replace")
    match = re.search(r"DXGI targets: Present=([0-9A-F]+)", log)
    if not match:
        raise SystemExit("Present target was not found in the log")
    address = int(match.group(1), 16)
    module_list = modules(pid)
    prehook_match = re.search(r"Present prehook bytes: ([0-9A-F ]+)", log)
    if prehook_match:
        prehook = bytes.fromhex(prehook_match.group(1))
        destination = next_jump(process, address, prehook)
        if destination:
            source = address
            for depth in range(8):
                destination_code = read(process, destination, 16)
                print(
                    f"[prehook {depth}] 0x{source:016X} -> 0x{destination:016X} "
                    f"{module_name(module_list, destination)} "
                    f"bytes={destination_code.hex(' ') if destination_code else 'unreadable'}"
                )
                if not destination_code:
                    break
                following = next_jump(process, destination, destination_code)
                if not following or following == destination:
                    break
                source, destination = destination, following
    visited = set()
    for depth in range(8):
        if not address or address in visited:
            break
        visited.add(address)
        code = read(process, address, 16)
        if not code:
            break
        print(
            f"[{depth}] 0x{address:016X} {module_name(module_list, address)} "
            f"bytes={code.hex(' ')}"
        )
        address = next_jump(process, address, code)
    kernel32.CloseHandle(process)


if __name__ == "__main__":
    main()
