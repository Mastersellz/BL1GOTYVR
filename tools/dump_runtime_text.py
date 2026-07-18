"""Create an analysis-only PE copy with executable sections replaced from live memory."""

from pathlib import Path
import re
import struct

from inspect_ue3_camera_properties import find_pid, kernel32, read_memory


IMAGE = Path(
    r"F:\SteamLibrary\steamapps\common\BorderlandsGOTYEnhanced\Binaries\Win64\BorderlandsGOTY.exe"
)
LOG = Path(
    r"F:\SteamLibrary\steamapps\common\BorderlandsGOTYEnhanced\Binaries\Win64\BL1GOTYVR.log"
)
OUTPUT = Path(__file__).resolve().parents[1] / "build" / "BorderlandsGOTY_runtime.exe"
IMAGE_SCN_MEM_EXECUTE = 0x20000000


def main():
    pid = find_pid()
    if not pid:
        raise SystemExit("BorderlandsGOTY.exe is not running")
    process = kernel32.OpenProcess(0x0010 | 0x0400, False, pid)
    if not process:
        raise SystemExit("OpenProcess failed")
    log = LOG.read_text(encoding="utf-8", errors="replace")
    match = re.search(r"\[UE3Scanner\] Module: ([0-9A-F]+)", log)
    if not match:
        raise SystemExit("Runtime module base was not found in the log")
    module_base = int(match.group(1), 16)

    image = bytearray(IMAGE.read_bytes())
    pe = struct.unpack_from("<I", image, 0x3C)[0]
    section_count = struct.unpack_from("<H", image, pe + 6)[0]
    optional_size = struct.unpack_from("<H", image, pe + 20)[0]
    section_table = pe + 24 + optional_size
    for index in range(section_count):
        entry = section_table + index * 40
        name = image[entry:entry + 8].rstrip(b"\0").decode(errors="replace")
        virtual_size, rva, raw_size, raw = struct.unpack_from("<IIII", image, entry + 8)
        characteristics = struct.unpack_from("<I", image, entry + 36)[0]
        if not characteristics & IMAGE_SCN_MEM_EXECUTE or not raw_size:
            continue
        size = min(virtual_size, raw_size)
        runtime = read_memory(process, module_base + rva, size)
        if not runtime:
            print(f"Skipped {name}: ReadProcessMemory failed")
            continue
        image[raw:raw + size] = runtime
        print(f"Patched {name}: RVA 0x{rva:X}, {size:,} bytes")

    OUTPUT.parent.mkdir(parents=True, exist_ok=True)
    OUTPUT.write_bytes(image)
    kernel32.CloseHandle(process)
    print(f"Wrote {OUTPUT}")


if __name__ == "__main__":
    main()
