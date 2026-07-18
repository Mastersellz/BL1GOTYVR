"""Inspect camera-related UE3 reflection objects in a running BL1 GOTY process."""

import ctypes
import ctypes.wintypes as wintypes
import re
import struct
from pathlib import Path


PROCESS_VM_READ = 0x0010
PROCESS_QUERY_INFORMATION = 0x0400
LOG_PATH = Path(
    r"F:\SteamLibrary\steamapps\common\BorderlandsGOTYEnhanced\Binaries\Win64\BL1GOTYVR.log"
)
TARGETS = {
    "Camera",
    "CameraCache",
    "TCameraCache",
    "WillowPlayerCamera",
    "WillowPlayerController",
    "PlayerCameraLoc",
    "PlayerCameraPosition",
    "PlayerCameraRot",
    "PlayerCameraDirection",
    "PlayerCamera",
}

kernel32 = ctypes.WinDLL("kernel32", use_last_error=True)


def find_pid():
    snapshot = kernel32.CreateToolhelp32Snapshot(0x00000002, 0)
    if snapshot == ctypes.c_void_p(-1).value:
        return None

    class ProcessEntry(ctypes.Structure):
        _fields_ = [
            ("dwSize", wintypes.DWORD),
            ("cntUsage", wintypes.DWORD),
            ("th32ProcessID", wintypes.DWORD),
            ("th32DefaultHeapID", ctypes.c_size_t),
            ("th32ModuleID", wintypes.DWORD),
            ("cntThreads", wintypes.DWORD),
            ("th32ParentProcessID", wintypes.DWORD),
            ("pcPriClassBase", wintypes.LONG),
            ("dwFlags", wintypes.DWORD),
            ("szExeFile", ctypes.c_char * 260),
        ]

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


def read_memory(process, address, size):
    buffer = ctypes.create_string_buffer(size)
    read = ctypes.c_size_t()
    if not kernel32.ReadProcessMemory(
        process, ctypes.c_void_p(address), buffer, size, ctypes.byref(read)
    ) or read.value != size:
        return None
    return buffer.raw


def read_qword(process, address):
    data = read_memory(process, address, 8)
    return struct.unpack("<Q", data)[0] if data else 0


def read_name(process, names_data, name_count, index):
    if index < 0 or index >= name_count:
        return None
    entry = read_qword(process, names_data + index * 8)
    if not entry:
        return None
    data = read_memory(process, entry + 0x14, 128)
    if not data:
        return None
    return data.split(b"\0", 1)[0].decode("ascii", errors="replace")


def object_name(process, names_data, name_count, obj):
    data = read_memory(process, obj + 0x48, 8)
    if not data:
        return None
    name_index, name_number = struct.unpack("<ii", data)
    name = read_name(process, names_data, name_count, name_index)
    if not name:
        return None
    return f"{name}_{name_number - 1}" if name_number else name


def describe_object(process, names_data, name_count, obj):
    class_obj = read_qword(process, obj + 0x50)
    outer_obj = read_qword(process, obj + 0x40)
    return (
        object_name(process, names_data, name_count, obj),
        object_name(process, names_data, name_count, class_obj),
        object_name(process, names_data, name_count, outer_obj),
    )


def main():
    pid = find_pid()
    if not pid:
        raise SystemExit("BorderlandsGOTY.exe is not running")
    process = kernel32.OpenProcess(
        PROCESS_VM_READ | PROCESS_QUERY_INFORMATION, False, pid
    )
    if not process:
        raise SystemExit(f"OpenProcess failed: {ctypes.get_last_error()}")

    log = LOG_PATH.read_text(encoding="utf-8", errors="replace")
    names_match = re.search(r"GNames candidate at ([0-9A-F]+): count=(\d+)", log)
    objects_match = re.search(r"GObjects candidate at ([0-9A-F]+): count=(\d+)", log)
    if not names_match or not objects_match:
        raise SystemExit("Validated GNames/GObjects were not found in the current log")

    names_array = int(names_match.group(1), 16)
    objects_array = int(objects_match.group(1), 16)
    names_header = read_memory(process, names_array, 16)
    objects_header = read_memory(process, objects_array, 16)
    if not names_header or not objects_header:
        raise SystemExit("Could not read UE3 array headers")
    names_data, name_count, _ = struct.unpack("<Qii", names_header)
    objects_data, object_count, _ = struct.unpack("<Qii", objects_header)

    target_indices = {}
    for index in range(name_count):
        name = read_name(process, names_data, name_count, index)
        if name in TARGETS:
            target_indices[index] = name
    print(f"PID={pid} GNames={name_count} GObjects={object_count}")
    print("Target FNames:", target_indices)

    pointers = read_memory(process, objects_data, object_count * 8)
    if not pointers:
        raise SystemExit("Could not read GObjects pointer array")
    object_pointers = [item[0] for item in struct.iter_unpack("<Q", pointers)]
    for index, obj in enumerate(object_pointers):
        if not obj:
            continue
        fname = read_memory(process, obj + 0x48, 8)
        if not fname:
            continue
        name_index, _ = struct.unpack("<ii", fname)
        if name_index not in target_indices:
            continue
        name, class_name, outer_name = describe_object(
            process, names_data, name_count, obj
        )
        print(
            f"\nGObjects[{index}] 0x{obj:016X}: name={name} "
            f"class={class_name} outer={outer_name}"
        )
        if class_name and class_name.endswith("Property"):
            property_data = read_memory(process, obj + 0x68, 0x28)
            if property_data:
                array_dim, element_size = struct.unpack_from("<ii", property_data, 0)
                property_offset = struct.unpack_from("<i", property_data, 0x24)[0]
                print(
                    f"  PROPERTY_SUMMARY name={name} outer={outer_name} class={class_name} "
                    f"arrayDim={array_dim} elementSize=0x{element_size:X} "
                    f"offset=0x{property_offset:X}"
                )
        data = read_memory(process, obj + 0x58, 0xE8)
        if not data:
            continue
        for row in range(0, len(data) - 15, 16):
            values = struct.unpack_from("<IIII", data, row)
            print(
                f"  +0x{0x58 + row:03X}: "
                + " ".join(f"{value:08X}" for value in values)
            )
        print("  UObject pointer fields:")
        for pointer_offset in range(0x58, 0xD0, 8):
            pointed = read_qword(process, obj + pointer_offset)
            pointed_name = object_name(process, names_data, name_count, pointed)
            if not pointed_name:
                continue
            _, pointed_class, pointed_outer = describe_object(
                process, names_data, name_count, pointed
            )
            print(
                f"    +0x{pointer_offset:03X} -> 0x{pointed:016X} "
                f"name={pointed_name} class={pointed_class} outer={pointed_outer}"
            )

    camera_class_cache = {}

    def derives_from_camera(class_obj):
        chain = []
        current = class_obj
        result = False
        for _ in range(16):
            if not current:
                break
            if current in camera_class_cache:
                result = camera_class_cache[current]
                break
            chain.append(current)
            if object_name(process, names_data, name_count, current) == "Camera":
                result = True
                break
            current = read_qword(process, current + 0x78)
        for item in chain:
            camera_class_cache[item] = result
        return result

    print("\nLive Camera-derived instances:")
    for index, obj in enumerate(object_pointers):
        if not obj:
            continue
        class_obj = read_qword(process, obj + 0x50)
        if not derives_from_camera(class_obj):
            continue
        name, class_name, outer_name = describe_object(
            process, names_data, name_count, obj
        )
        if not name or name.startswith("Default__"):
            continue
        cache = read_memory(process, obj + 0x334, 0x20)
        if not cache:
            continue
        timestamp, x, y, z, pitch, yaw, roll, fov = struct.unpack("<ffffiiif", cache)
        print(
            f"  GObjects[{index}] 0x{obj:016X} name={name} class={class_name} "
            f"outer={outer_name} cache=(t={timestamp:.3f} loc=({x:.2f},{y:.2f},{z:.2f}) "
            f"rot=({pitch},{yaw},{roll}) fov={fov:.2f})"
        )

    print("\nCamera-derived pointers in live WillowPlayerController instances:")
    active_controller_class = 0
    for index, obj in enumerate(object_pointers):
        if not obj:
            continue
        name, class_name, outer_name = describe_object(
            process, names_data, name_count, obj
        )
        if class_name != "WillowPlayerController" or not name or name.startswith("Default__"):
            continue
        active_controller_class = read_qword(process, obj + 0x50)
        calc_view = read_memory(process, obj + 0xCA8, 24)
        cached_fov = read_memory(process, obj + 0xF48, 4)
        if calc_view and cached_fov:
            x, y, z, pitch, yaw, roll = struct.unpack("<fffiii", calc_view)
            (fov,) = struct.unpack("<f", cached_fov)
            print(
                f"  CALC_VIEW controller=0x{obj:016X} loc=({x:.2f},{y:.2f},{z:.2f}) "
                f"rot=({pitch},{yaw},{roll}) cachedFov={fov:.2f}"
            )
        reflected_camera = read_qword(process, obj + 0x6D4)
        reflected_name, reflected_class, reflected_outer = describe_object(
            process, names_data, name_count, reflected_camera
        )
        reflected_cache = read_memory(process, reflected_camera + 0x334, 0x20)
        print(
            f"  REFLECTED_PLAYERCAMERA controller=0x{obj:016X} +0x6D4="
            f"0x{reflected_camera:016X} name={reflected_name} class={reflected_class} "
            f"outer={reflected_outer} cache={reflected_cache.hex() if reflected_cache else None}"
        )
        for offset in range(0x80, 0x1900, 4):
            pointed = read_qword(process, obj + offset)
            if not pointed:
                continue
            pointed_class = read_qword(process, pointed + 0x50)
            if not derives_from_camera(pointed_class):
                continue
            pointed_name, pointed_class_name, pointed_outer = describe_object(
                process, names_data, name_count, pointed
            )
            cache = read_memory(process, pointed + 0x334, 0x20)
            if not pointed_name or not cache:
                continue
            timestamp, x, y, z, pitch, yaw, roll, fov = struct.unpack(
                "<ffffiiif", cache
            )
            print(
                f"  controller=0x{obj:016X} +0x{offset:X} -> 0x{pointed:016X} "
                f"name={pointed_name} class={pointed_class_name} outer={pointed_outer} "
                f"cache=(t={timestamp:.3f} loc=({x:.2f},{y:.2f},{z:.2f}) "
                f"rot=({pitch},{yaw},{roll}) fov={fov:.2f})"
            )

    controller_classes = set()
    current_class = active_controller_class
    while current_class and current_class not in controller_classes:
        controller_classes.add(current_class)
        current_class = read_qword(process, current_class + 0x78)
    print("\nReflected camera/view properties in the PlayerController hierarchy:")
    property_tokens = ("camera", "view", "pov", "fov", "location", "rotation")
    for index, obj in enumerate(object_pointers):
        if not obj or read_qword(process, obj + 0x40) not in controller_classes:
            continue
        name, class_name, outer_name = describe_object(
            process, names_data, name_count, obj
        )
        if not name or not class_name or not class_name.endswith("Property") or not any(
            token in name.lower() for token in property_tokens
        ):
            continue
        property_data = read_memory(process, obj + 0x68, 0x28)
        if not property_data:
            continue
        _, element_size = struct.unpack_from("<ii", property_data, 0)
        property_offset = struct.unpack_from("<i", property_data, 0x24)[0]
        print(
            f"  CONTROLLER_PROPERTY name={name} outer={outer_name} class={class_name} "
            f"elementSize=0x{element_size:X} offset=0x{property_offset:X}"
        )

    kernel32.CloseHandle(process)


if __name__ == "__main__":
    main()
