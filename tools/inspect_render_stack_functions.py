"""Map captured render-stack return RVAs to PE64 runtime functions."""

from pathlib import Path
import ctypes
import re
import struct

from inspect_ue3_camera_properties import find_pid, kernel32, read_memory


IMAGE = Path(
    r"F:\SteamLibrary\steamapps\common\BorderlandsGOTYEnhanced\Binaries\Win64\BorderlandsGOTY.exe"
)
RETURNS = [
    0x122F6AD,
    0x481888,
    0x468BB8,
    0x46D77F,
    0x4578BC,
    0x1237F0E,
    0x123700F,
    0x59062B,
    0x59122F,
    0x435ECE,
    0x436A19,
    0x1F7CB0,
]


def main():
    image = IMAGE.read_bytes()
    pe = struct.unpack_from("<I", image, 0x3C)[0]
    section_count = struct.unpack_from("<H", image, pe + 6)[0]
    optional_size = struct.unpack_from("<H", image, pe + 20)[0]
    section_table = pe + 24 + optional_size
    sections = []
    for index in range(section_count):
        entry = section_table + index * 40
        name = image[entry:entry + 8].rstrip(b"\0").decode(errors="replace")
        virtual_size, rva, raw_size, raw = struct.unpack_from("<IIII", image, entry + 8)
        sections.append((name, rva, virtual_size, raw, raw_size))

    def rva_to_raw(rva):
        for _, start, virtual_size, raw, raw_size in sections:
            if start <= rva < start + max(virtual_size, raw_size):
                delta = rva - start
                return raw + delta if delta < raw_size else None
        return None

    pdata = next(section for section in sections if section[0] == ".pdata")
    runtime_functions = []
    for offset in range(pdata[3], pdata[3] + pdata[4] - 11, 12):
        begin, end, unwind = struct.unpack_from("<III", image, offset)
        if begin < end:
            runtime_functions.append((begin, end, unwind))

    process = None
    module_base = 0
    pid = find_pid()
    if pid:
        process = kernel32.OpenProcess(0x0010 | 0x0400, False, pid)
        log_path = Path(
            r"F:\SteamLibrary\steamapps\common\BorderlandsGOTYEnhanced\Binaries\Win64\BL1GOTYVR.log"
        )
        log = log_path.read_text(encoding="utf-8", errors="replace")
        match = re.search(r"\[UE3Scanner\] Module: ([0-9A-F]+)", log)
        if match:
            module_base = int(match.group(1), 16)

    for return_rva in RETURNS:
        function = next(
            (item for item in runtime_functions if item[0] <= return_rva < item[1]), None
        )
        if not function:
            print(f"return=0x{return_rva:08X}: no runtime function")
            continue
        begin, end, unwind = function
        raw = rva_to_raw(begin)
        prefix = image[raw:raw + min(64, end - begin)].hex(" ") if raw is not None else ""
        print(
            f"return=0x{return_rva:08X} function=0x{begin:08X}-0x{end:08X} "
            f"size=0x{end - begin:X} returnOffset=0x{return_rva - begin:X} "
            f"unwind=0x{unwind:08X}\n  bytes={prefix}"
        )
        if process and module_base:
            runtime = read_memory(process, module_base + begin, min(96, end - begin))
            if runtime:
                print(f"  runtime={runtime.hex(' ')}")

    if process:
        kernel32.CloseHandle(process)


if __name__ == "__main__":
    main()
