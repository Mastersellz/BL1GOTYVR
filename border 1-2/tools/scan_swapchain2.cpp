#include <windows.h>
#include <Psapi.h>
#include <d3d11.h>
#include <dxgi.h>
#include <stdio.h>

#pragma comment(lib, "psapi.lib")

static void Log(const char* fmt, ...) {
    char buf[512];
    va_list args;
    va_start(args, fmt);
    vsnprintf(buf, sizeof(buf), fmt, args);
    va_end(args);
    OutputDebugStringA(buf);
    FILE* f = fopen("F:\\SteamLibrary\\steamapps\\common\\BorderlandsGOTYEnhanced\\Binaries\\Win64\\hook_scan.txt", "a");
    if (f) { fprintf(f, "%s\n", buf); fclose(f); }
}

static DWORD WINAPI ScanThread(LPVOID) {
    Log("=== Internal swapchain scan started ===");

    HMODULE hDXGI = GetModuleHandleA("dxgi.dll");
    if (!hDXGI) { Log("dxgi.dll not found"); return 1; }
    MODULEINFO dxgiInfo;
    GetModuleInformation(GetCurrentProcess(), hDXGI, &dxgiInfo, sizeof(dxgiInfo));
    uintptr_t dxgiBase = (uintptr_t)dxgiInfo.lpBaseOfDll;
    uintptr_t dxgiEnd = dxgiBase + dxgiInfo.SizeOfImage;
    Log("dxgi.dll: %p - %p", (void*)dxgiBase, (void*)dxgiEnd);

    // Scan ALL committed readable memory
    int found = 0;
    uintptr_t addr = 0;
    MEMORY_BASIC_INFORMATION mbi;

    while (VirtualQuery((LPCVOID)addr, &mbi, sizeof(mbi))) {
        if (mbi.State == 0x1000 &&  // MEM_COMMIT
            (mbi.Protect & (PAGE_READWRITE | PAGE_WRITECOPY | PAGE_EXECUTE_READWRITE)) &&
            mbi.RegionSize > 0 && mbi.RegionSize < 0x1000000) {

            uintptr_t regionStart = (uintptr_t)mbi.BaseAddress;
            uintptr_t regionEnd = regionStart + mbi.RegionSize;

            for (uintptr_t p = regionStart; p + 8 <= regionEnd; p += 8) {
                uintptr_t value = 0;
                if (!ReadProcessMemory(GetCurrentProcess(), (LPCVOID)p, &value, sizeof(value), nullptr))
                    continue;
                if (value < dxgiBase || value >= dxgiEnd || (value & 7))
                    continue;

                // Check vtable[8] = Present
                uintptr_t present = 0;
                if (!ReadProcessMemory(GetCurrentProcess(), (LPCVOID)(value + 8*8), &present, sizeof(present), nullptr))
                    continue;
                if (present < dxgiBase || present >= dxgiEnd)
                    continue;

                // Verify vtable density
                int hits = 0;
                for (int j = 0; j < 25; j++) {
                    uintptr_t entry = 0;
                    if (ReadProcessMemory(GetCurrentProcess(), (LPCVOID)(value + j*8), &entry, sizeof(entry), nullptr))
                        if (entry >= dxgiBase && entry < dxgiEnd) hits++;
                }

                if (hits >= 20) {
                    Log("VTable %p (from %p): Present=%p (%d/25)", (void*)value, (void*)p, (void*)present, hits);
                    found++;
                }
            }
        }

        uintptr_t next = (uintptr_t)mbi.BaseAddress + mbi.RegionSize;
        if (next <= addr) break;
        addr = next;
    }

    Log("=== Found %d vtable candidates ===", found);
    return 0;
}

BOOL APIENTRY DllMain(HMODULE, DWORD reason, LPVOID) {
    if (reason == DLL_PROCESS_ATTACH) {
        DisableThreadLibraryCalls(GetModuleHandleA(NULL));
        CreateThread(NULL, 0, ScanThread, NULL, 0, NULL);
    }
    return TRUE;
}
