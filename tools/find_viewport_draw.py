"""Locate UE3 UGameViewportClient::Draw candidates in a PE64 image."""

from pathlib import Path
import struct


IMAGE = Path(r"F:\SteamLibrary\steamapps\common\BorderlandsGOTYEnhanced\Binaries\Win64\BorderlandsGOTY.exe")
ME_IMAGE = Path(r"F:\Mass Effect Legendary Edition\Game\ME1\Binaries\Win64\MassEffect1.exe")


def parse_pattern(text: str):
    values = []
    mask = []
    for token in text.split():
        if token == "??":
            values.append(0)
            mask.append(False)
        else:
            values.append(int(token, 16))
            mask.append(True)
    return bytes(values), mask


def find_pattern(data: bytes, text: str):
    pattern, mask = parse_pattern(text)
    size = len(pattern)
    for offset in range(len(data) - size + 1):
        if all(not mask[i] or data[offset + i] == pattern[i] for i in range(size)):
            yield offset


def inspect_viewport_vtables(path: Path, known_draw_rva=None):
    image = path.read_bytes()
    pe_offset = struct.unpack_from("<I", image, 0x3C)[0]
    section_count = struct.unpack_from("<H", image, pe_offset + 6)[0]
    optional_size = struct.unpack_from("<H", image, pe_offset + 20)[0]
    optional = pe_offset + 24
    image_base = struct.unpack_from("<Q", image, optional + 24)[0]
    image_size = struct.unpack_from("<I", image, optional + 56)[0]
    section_table = optional + optional_size
    sections = []
    for index in range(section_count):
        section = section_table + index * 40
        name = image[section:section + 8].rstrip(b"\0").decode("ascii", errors="replace")
        virtual_size, virtual_address, raw_size, raw_offset = struct.unpack_from(
            "<IIII", image, section + 8
        )
        sections.append((name, virtual_address, virtual_size, raw_offset, raw_size))
    _, text_rva, text_size, _, _ = next(section for section in sections if section[0] == ".text")

    def raw_to_rva(raw_offset):
        for _, rva, _, section_raw, raw_size in sections:
            if section_raw <= raw_offset < section_raw + raw_size:
                return rva + raw_offset - section_raw
        return None

    def rva_to_raw(rva):
        for _, section_rva, virtual_size, section_raw, raw_size in sections:
            if section_rva <= rva < section_rva + max(virtual_size, raw_size):
                delta = rva - section_rva
                return section_raw + delta if delta < raw_size else None
        return None

    print(f"\nRTTI vtables: {path.name} imageBase=0x{image_base:X}")
    if known_draw_rva is not None:
        pdata = next((section for section in sections if section[0] == ".pdata"), None)
        draw_end_rva = None
        if pdata:
            _, _, _, pdata_raw, pdata_raw_size = pdata
            for offset in range(pdata_raw, pdata_raw + pdata_raw_size - 11, 12):
                begin, end, _ = struct.unpack_from("<III", image, offset)
                if begin <= known_draw_rva < end:
                    draw_end_rva = end
                    print(f"  known Draw function RVA 0x{begin:08X}-0x{end:08X} size=0x{end-begin:X}")
                    break
        if draw_end_rva is not None:
            draw_raw = rva_to_raw(known_draw_rva)
            body = image[draw_raw:draw_raw + draw_end_rva - known_draw_rva]
            strings = set()
            for offset in range(len(body) - 7):
                if body[offset] not in (0x48, 0x4C) or body[offset + 1] != 0x8D:
                    continue
                if body[offset + 2] & 0xC7 != 0x05:
                    continue
                displacement = struct.unpack_from("<i", body, offset + 3)[0]
                target_rva = known_draw_rva + offset + 7 + displacement
                target_raw = rva_to_raw(target_rva)
                if target_raw is None:
                    continue
                value = image[target_raw:target_raw + 160].split(b"\0", 1)[0]
                if len(value) >= 4 and all(0x20 <= byte < 0x7F for byte in value):
                    strings.add((target_rva, value.decode("ascii")))
            print("  known Draw RIP strings:")
            for string_rva, value in sorted(strings):
                print(f"    0x{string_rva:08X}: {value}")
        draw_pointer = struct.pack("<Q", image_base + known_draw_rva)
        search = 0
        while True:
            raw = image.find(draw_pointer, search)
            if raw < 0:
                break
            search = raw + 1
            pointer_rva = raw_to_rva(raw)
            print(f"  known Draw pointer at RVA 0x{pointer_rva:08X}")
            _, text_section_rva, _, text_raw, text_raw_size = next(
                section for section in sections if section[0] == ".text"
            )
            text = image[text_raw:text_raw + text_raw_size]
            print("  constructor-style LEAs into surrounding vtable:")
            for offset in range(len(text) - 7):
                if text[offset:offset + 3] != b"\x48\x8d\x05":
                    continue
                displacement = struct.unpack_from("<i", text, offset + 3)[0]
                instruction_rva = text_section_rva + offset
                target_rva = instruction_rva + 7 + displacement
                delta = pointer_rva - target_rva
                if 0 <= delta <= 0x4000 and delta % 8 == 0:
                    print(f"    code RVA 0x{instruction_rva:08X} -> vtable RVA 0x{target_rva:08X}; "
                          f"Draw slot {delta // 8}")
            for relative_slot in range(-64, 33):
                entry_raw = raw + relative_slot * 8
                pointer = struct.unpack_from("<Q", image, entry_raw)[0]
                function_rva = pointer - image_base
                in_text = text_rva <= function_rva < text_rva + text_size
                marker = "code" if in_text else "----"
                print(f"    [{relative_slot:+03d}] 0x{function_rva:016X} {marker}")
    search_at = 0
    seen_type_descriptors = set()
    while True:
        match = image.find(b"UGameViewportClient", search_at)
        if match < 0:
            break
        search_at = match + 1
        string_start = image.rfind(b"\0", max(0, match - 64), match) + 1
        string_end = image.find(b"\0", match)
        if string_end < 0:
            continue
        decorated = image[string_start:string_end]
        if not decorated.startswith(b".?A"):
            print(f"  non-RTTI string near match: {decorated[:120]!r}")
            continue
        type_raw = string_start - 16
        type_rva = raw_to_rva(type_raw)
        if type_rva is None or type_rva in seen_type_descriptors:
            continue
        seen_type_descriptors.add(type_rva)
        print(f"  type {decorated.decode(errors='replace')} RVA 0x{type_rva:08X}")

        encoded_type_rva = struct.pack("<I", type_rva)
        col_search = 0
        while True:
            type_ref_raw = image.find(encoded_type_rva, col_search)
            if type_ref_raw < 0:
                break
            col_search = type_ref_raw + 1
            col_raw = type_ref_raw - 12
            col_rva = raw_to_rva(col_raw)
            if col_rva is None or col_raw < 0 or col_raw + 24 > len(image):
                continue
            signature, subobject_offset, cd_offset, td, class_desc, self_rva = struct.unpack_from(
                "<IIIIII", image, col_raw
            )
            if signature != 1 or td != type_rva or self_rva != col_rva:
                continue
            absolute_col = struct.pack("<Q", image_base + col_rva)
            pointer_search = 0
            while True:
                col_pointer_raw = image.find(absolute_col, pointer_search)
                if col_pointer_raw < 0:
                    break
                pointer_search = col_pointer_raw + 1
                vtable_raw = col_pointer_raw + 8
                vtable_rva = raw_to_rva(vtable_raw)
                if vtable_rva is None:
                    continue
                print(f"    COL RVA 0x{col_rva:08X} subobject=0x{subobject_offset:X} "
                      f"vtable RVA 0x{vtable_rva:08X}")
                for slot in range(64):
                    pointer = struct.unpack_from("<Q", image, vtable_raw + slot * 8)[0]
                    function_rva = pointer - image_base
                    if function_rva < 0 or function_rva >= image_size or rva_to_raw(function_rva) is None:
                        break
                    marker = " <== known Draw" if function_rva == known_draw_rva else ""
                    print(f"      [{slot:02d}] RVA 0x{function_rva:08X}{marker}")


def main():
    inspect_viewport_vtables(ME_IMAGE, 0x004C71A0)
    inspect_viewport_vtables(IMAGE)

    image = IMAGE.read_bytes()
    pe_offset = struct.unpack_from("<I", image, 0x3C)[0]
    section_count = struct.unpack_from("<H", image, pe_offset + 6)[0]
    optional_size = struct.unpack_from("<H", image, pe_offset + 20)[0]
    section_table = pe_offset + 24 + optional_size
    sections = []
    for index in range(section_count):
        section = section_table + index * 40
        name = image[section:section + 8].rstrip(b"\0").decode("ascii", errors="replace")
        virtual_size, virtual_address, raw_size, raw_offset = struct.unpack_from(
            "<IIII", image, section + 8
        )
        sections.append((name, virtual_address, virtual_size, raw_offset, raw_size))

    def rva_to_raw(rva):
        for _, section_rva, virtual_size, section_raw, raw_size in sections:
            if section_rva <= rva < section_rva + max(virtual_size, raw_size):
                delta = rva - section_rva
                return section_raw + delta if delta < raw_size else None
        return None

    text_offset = text_rva = text_size = 0
    for index in range(section_count):
        section = section_table + index * 40
        name = image[section:section + 8].rstrip(b"\0")
        virtual_size, virtual_address, raw_size, raw_offset = struct.unpack_from(
            "<IIII", image, section + 8
        )
        if name == b".text":
            text_offset = raw_offset
            text_rva = virtual_address
            text_size = raw_size
            break

    text = image[text_offset:text_offset + text_size]

    pdata = next((section for section in sections if section[0] == ".pdata"), None)
    runtime_functions = []
    if pdata:
        _, _, _, pdata_raw, pdata_raw_size = pdata
        for offset in range(pdata_raw, pdata_raw + pdata_raw_size - 11, 12):
            begin, end, _ = struct.unpack_from("<III", image, offset)
            if begin < end:
                runtime_functions.append((begin, end))

    field_matches = []
    field = struct.pack("<I", 0x7C8)
    start = 0
    while True:
        match = text.find(field, start)
        if match < 0:
            break
        instruction_rva = text_rva + match
        containing = next(((begin, end) for begin, end in runtime_functions
                           if begin <= instruction_rva < end), None)
        if containing and containing not in field_matches:
            field_matches.append(containing)
        start = match + 1
    print(f"\nFunctions referencing displacement 0x7C8: {len(field_matches)}")
    for begin, end in sorted(field_matches, key=lambda item: item[1] - item[0], reverse=True)[:100]:
        raw = rva_to_raw(begin)
        prefix = image[raw:raw + 32].hex(" ") if raw is not None else ""
        print(f"  RVA 0x{begin:08X}-0x{end:08X} size=0x{end-begin:X} bytes={prefix}")

    print("\nLargest runtime functions:")
    for begin, end in sorted(runtime_functions, key=lambda item: item[1] - item[0], reverse=True)[:80]:
        raw = rva_to_raw(begin)
        body = image[raw:raw + end - begin] if raw is not None else b""
        offsets = [value for value in (0x120, 0x161, 0x7C8) if struct.pack("<I", value) in body]
        print(f"  RVA 0x{begin:08X}-0x{end:08X} size=0x{end-begin:X} offsets={offsets}")
    anchors = {
        "GEngine->GameViewport(+0x7C8)":
            "48 8B 05 ?? ?? ?? ?? 48 8B ?? C8 07 00 00",
        "ME-style Draw prologue":
            "48 8B C4 55 56 57 41 54 41 55 41 56 41 57 48 8D A8 ?? ?? ?? ?? 48 81 EC",
        "Draw parameter preservation":
            "4D 8B E8 4C 89 45 ?? 4C 8B E2 48 89 95 ?? ?? ?? ?? 48 8B F9",
        "Viewport multi-vtable constructor":
            "48 8D 05 ?? ?? ?? ?? 48 89 03 48 8D 05 ?? ?? ?? ?? 48 89 43 60 "
            "48 8D 05 ?? ?? ?? ?? 48 89 43 68 48 8D 05 ?? ?? ?? ?? 48 89 43 70",
    }

    print(f"Image: {IMAGE}")
    print(f".text RVA=0x{text_rva:X} raw=0x{text_offset:X} size=0x{text_size:X}")
    for name, pattern in anchors.items():
        matches = list(find_pattern(text, pattern))
        print(f"\n{name}: {len(matches)} match(es)")
        for match in matches[:100]:
            print(f"  RVA 0x{text_rva + match:08X}")
            if name == "Viewport multi-vtable constructor":
                instruction_rva = text_rva + match + 10
                displacement = struct.unpack_from("<i", text, match + 13)[0]
                vtable_rva = instruction_rva + 7 + displacement
                vtable_raw = rva_to_raw(vtable_rva)
                if vtable_raw is not None:
                    draw_pointer = struct.unpack_from("<Q", image, vtable_raw + 16)[0]
                    draw_rva = draw_pointer - 0x140000000
                    containing = next(((begin, end) for begin, end in runtime_functions
                                       if begin <= draw_rva < end), None)
                    size = containing[1] - containing[0] if containing else 0
                    print(f"    secondary vtable RVA 0x{vtable_rva:08X}, slot 2 RVA "
                          f"0x{draw_rva:08X}, function size=0x{size:X}")

    print("\nGeneric +0x60/+0x68/+0x70 vtable-store sequences:")
    generic_candidates = []
    for offset in range(7, len(text) - 128):
        if text[offset:offset + 2] != b"\x48\x89" or not 0x41 <= text[offset + 2] <= 0x47:
            continue
        if text[offset + 3] != 0x60 or text[offset - 7:offset - 4] != b"\x48\x8d\x05":
            continue
        register = text[offset + 2]
        tail = text[offset + 4:offset + 128]
        if bytes((0x48, 0x89, register, 0x68)) not in tail:
            continue
        if bytes((0x48, 0x89, register, 0x70)) not in tail:
            continue
        lea_offset = offset - 7
        displacement = struct.unpack_from("<i", text, lea_offset + 3)[0]
        vtable_rva = text_rva + lea_offset + 7 + displacement
        vtable_raw = rva_to_raw(vtable_rva)
        if vtable_raw is None:
            continue
        draw_pointer = struct.unpack_from("<Q", image, vtable_raw + 16)[0]
        draw_rva = draw_pointer - 0x140000000
        containing = next(((begin, end) for begin, end in runtime_functions
                           if begin <= draw_rva < end), None)
        size = containing[1] - containing[0] if containing else 0
        generic_candidates.append((text_rva + lea_offset, vtable_rva, draw_rva, size))
    for code_rva, vtable_rva, draw_rva, size in generic_candidates:
        print(f"  code RVA 0x{code_rva:08X}, vtable RVA 0x{vtable_rva:08X}, "
              f"slot 2 RVA 0x{draw_rva:08X}, function size=0x{size:X}")

    def raw_to_rva(raw_offset):
        for _, rva, _, section_raw, raw_size in sections:
            if section_raw <= raw_offset < section_raw + raw_size:
                return rva + raw_offset - section_raw
        return None

    print("\nUE3 strings and RIP-relative references:")
    string_rvas = {}
    for term in ("UGameViewportClient", "GameViewportClient", "CalcSceneView",
                 "LocalPlayer", "PlayerCamera", "CalcCamera"):
        encodings = (term.encode("ascii"), term.encode("utf-16le"))
        found = []
        for encoded in encodings:
            start = 0
            while True:
                raw = image.find(encoded, start)
                if raw < 0:
                    break
                rva = raw_to_rva(raw)
                if rva is not None:
                    found.append(rva)
                start = raw + 1
        print(f"  {term}: {', '.join(f'0x{rva:08X}' for rva in found[:20]) or 'not found'}")
        for rva in found[:20]:
            string_rvas.setdefault(rva, []).append(term)

    references = {rva: [] for rva in string_rvas}
    for offset in range(len(text) - 7):
        rex, opcode, modrm = text[offset:offset + 3]
        if rex not in range(0x48, 0x50) or opcode not in (0x8D, 0x8B) or (modrm & 0xC7) != 0x05:
            continue
        displacement = struct.unpack_from("<i", text, offset + 3)[0]
        instruction_rva = text_rva + offset
        target_rva = instruction_rva + 7 + displacement
        if target_rva in references:
            references[target_rva].append(instruction_rva)
    for target_rva, ref_rvas in references.items():
        for ref_rva in ref_rvas[:20]:
            print(f"    {string_rvas[target_rva]} reference at RVA 0x{ref_rva:08X}")


if __name__ == "__main__":
    main()
