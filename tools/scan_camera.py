"""BL1GOTY Camera Scanner — scan game .data section for camera cache patterns."""
import ctypes, ctypes.wintypes as wintypes, struct, sys, time, subprocess

kernel32 = ctypes.WinDLL('kernel32', use_last_error=True)
PROCESS_VM_READ = 0x0010
PROCESS_QUERY_INFORMATION = 0x0400
PROCESS_QUERY_LIMITED_INFORMATION = 0x1000

class MODULEENTRY32(ctypes.Structure):
    _fields_ = [("dwSize", wintypes.DWORD), ("th32ModuleID", wintypes.DWORD),
                 ("th32ProcessID", wintypes.DWORD), ("GlblcntUsage", wintypes.WORD),
                 ("ProccntUsage", wintypes.WORD), ("modBaseAddr", ctypes.c_void_p),
                 ("modBaseSize", wintypes.DWORD), ("hModule", ctypes.wintypes.HMODULE),
                 ("szModule", ctypes.c_char * 256), ("szExePath", ctypes.c_char * 260)]

class MODULEINFO(ctypes.Structure):
    _fields_ = [("lpBaseOfDll", ctypes.c_void_p), ("SizeOfImage", wintypes.DWORD),
                 ("EntryPoint", ctypes.c_void_p)]

class MEMORY_BASIC_INFORMATION(ctypes.Structure):
    _fields_ = [("BaseAddress", ctypes.c_void_p), ("AllocationBase", ctypes.c_void_p),
                 ("AllocationProtect", wintypes.DWORD), ("RegionSize", ctypes.c_size_t),
                 ("State", wintypes.DWORD), ("Protect", wintypes.DWORD), ("Type", wintypes.DWORD)]

def find_pid():
    r = subprocess.run(['powershell', '-Command',
        '(Get-Process BorderlandsGOTY -EA SilentlyContinue).Id'], capture_output=True, text=True)
    s = r.stdout.strip()
    return int(s) if s and s.isdigit() else None

def open_proc(pid):
    h = kernel32.OpenProcess(PROCESS_VM_READ | PROCESS_QUERY_INFORMATION, False, pid)
    if not h: raise RuntimeError(f"OpenProcess failed: {ctypes.get_last_error()}")
    return h

def read_mem(h, addr, sz):
    buf = ctypes.create_string_buffer(sz)
    n = ctypes.c_size_t(0)
    if kernel32.ReadProcessMemory(h, ctypes.c_void_p(addr), buf, sz, ctypes.byref(n)):
        return buf.raw[:n.value]
    return None

def get_module(h, name, pid):
    snap = kernel32.CreateToolhelp32Snapshot(0x00000008 | 0x00000010, pid)
    if snap == -1:
        return None, None
    me = MODULEENTRY32()
    me.dwSize = ctypes.sizeof(me)
    r = kernel32.Module32First(snap, ctypes.byref(me))
    while r:
        mod_name = me.szModule.decode('utf-8', errors='replace')
        if mod_name.lower() == name.lower():
            kernel32.CloseHandle(snap)
            return me.modBaseAddr, me.modBaseSize
        r = kernel32.Module32Next(snap, ctypes.byref(me))
    kernel32.CloseHandle(snap)
    return None, None

def get_writable_regions(h, start, size):
    """Get writable regions within a module."""
    regions = []
    addr = start
    end = start + size
    while addr < end:
        mbi = MEMORY_BASIC_INFORMATION()
        if not kernel32.VirtualQueryEx(h, ctypes.c_void_p(addr), ctypes.byref(mbi), ctypes.sizeof(mbi)):
            break
        if (mbi.State == 0x1000 and  # MEM_COMMIT
            mbi.Protect & (0x04 | 0x08 | 0x40 | 0x80)):  # RW/RWX
            regions.append((mbi.BaseAddress, min(mbi.RegionSize, end - addr)))
        next_addr = mbi.BaseAddress + mbi.RegionSize
        if next_addr <= addr: break
        addr = next_addr
    return regions

def scan_for_camera(h, regions):
    """Scan regions for float[3] position + int32[3] rotation + float FOV pattern."""
    candidates = []
    for base, size in regions:
        data = read_mem(h, base, size)
        if not data or len(data) < 32:
            continue
        for i in range(0, len(data) - 28, 4):
            try:
                loc = struct.unpack_from('<fff', data, i)
                if any(v != v or abs(v) > 100000 for v in loc): continue
                if all(v == 0.0 for v in loc): continue

                rot = struct.unpack_from('<iii', data, i + 12)
                if any(abs(v) > 131072 for v in rot): continue

                fov = struct.unpack_from('<f', data, i + 24)[0]
                if not (30.0 < fov < 170.0): continue

                addr = base + i
                candidates.append({'address': addr, 'loc': loc, 'rot': rot, 'fov': fov})
            except struct.error:
                continue
    return candidates

def score_candidates(cands, h, count=30):
    """Score top candidates by temporal stability."""
    for c in cands[:count]:
        d1 = read_mem(h, c['address'], 28)
        time.sleep(0.05)
        d2 = read_mem(h, c['address'], 28)
        if d1 and d2 and len(d1) >= 28 and len(d2) >= 28:
            loc1 = struct.unpack_from('<fff', d1, 0)
            loc2 = struct.unpack_from('<fff', d2, 0)
            rot1 = struct.unpack_from('<iii', d1, 12)
            rot2 = struct.unpack_from('<iii', d2, 12)
            fov1 = struct.unpack_from('<f', d1, 24)[0]
            fov2 = struct.unpack_from('<f', d2, 24)[0]
            ld = sum(abs(a-b) for a,b in zip(loc1, loc2))
            rd = sum(abs(a-b) for a,b in zip(rot1, rot2))
            fd = abs(fov1 - fov2)
            score = 100 - min(ld, 50) - min(rd*0.01, 30) + (50 if fd < 0.1 else 0)
            c['score'] = score
            c['loc'] = loc2
            c['rot'] = rot2
            c['fov'] = fov2
    return sorted(cands, key=lambda x: x.get('score', 0), reverse=True)

def main():
    pid = find_pid()
    if not pid:
        print("Game not found. Launch BorderlandsGOTY.exe first.")
        sys.exit(1)
    print(f"Game PID: {pid}")

    h = open_proc(pid)

    # Get game module — use known base from memengine analysis
    # The base address can change with ASLR, so we scan all writable regions
    # instead of relying on a fixed base.
    # Known previous base: 0x7FF722BE0000, size: 0x2959000
    # We'll scan ALL writable committed memory to be safe.
    base = None
    size = None
    print("Scanning all writable committed memory...")

    # Scan all writable committed regions
    regions = []
    addr = 0
    while addr < 0x7FFFFFFFFFFF:  # User-mode address space limit
        mbi = MEMORY_BASIC_INFORMATION()
        ret = kernel32.VirtualQueryEx(h, ctypes.c_void_p(addr), ctypes.byref(mbi), ctypes.sizeof(mbi))
        if not ret:
            break
        ba = mbi.BaseAddress if mbi.BaseAddress is not None else 0
        rs = mbi.RegionSize if mbi.RegionSize else 0
        if (mbi.State == 0x1000 and  # MEM_COMMIT
            mbi.Protect & (0x04 | 0x08 | 0x40 | 0x80) and  # RW/RWX
            rs > 0 and rs < 0x1000000):
            regions.append((ba, rs))
        next_addr = ba + rs
        if next_addr <= addr: break
        addr = next_addr

    print(f"Found {len(regions)} writable regions")

    total_bytes = sum(s for _, s in regions)
    print(f"Total bytes to scan: {total_bytes:,}")

    # Scan
    print("Scanning for camera patterns...")
    cands = scan_for_camera(h, regions)
    print(f"Found {len(cands)} raw candidates")

    if not cands:
        print("No candidates found. Game may need to be in a level.")
        kernel32.CloseHandle(h)
        sys.exit(1)

    # Score
    print("Scoring top candidates...")
    scored = score_candidates(cands, h)

    print(f"\nTop 10 camera candidates:")
    for i, c in enumerate(scored[:10]):
        s = c.get('score', 0)
        print(f"  #{i+1} @ 0x{c['address']:016X} (score={s:.1f})")
        print(f"      pos=({c['loc'][0]:.2f}, {c['loc'][1]:.2f}, {c['loc'][2]:.2f})")
        print(f"      rot=({c['rot'][0]}, {c['rot'][1]}, {c['rot'][2]})")
        print(f"      fov={c['fov']:.1f}")

    kernel32.CloseHandle(h)

if __name__ == "__main__":
    main()
