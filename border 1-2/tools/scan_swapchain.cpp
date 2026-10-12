#include <windows.h>
#include <Psapi.h>
#include <d3d11.h>
#include <dxgi.h>
#include <stdio.h>

#pragma comment(lib, "psapi.lib")

void Log(const char* fmt, ...) {
    char buf[512];
    va_list args;
    va_start(args, fmt);
    vsnprintf(buf, sizeof(buf), fmt, args);
    va_end(args);
    OutputDebugStringA(buf);
    FILE* f = fopen("F:\\SteamLibrary\\steamapps\\common\\BorderlandsGOTYEnhanced\\Binaries\\Win64\\hook_scan.txt", "a");
    if (f) { fprintf(f, "%s\n", buf); fclose(f); }
}

extern "C" __declspec(dllexport) void __cdecl ScanSwapchain() {
    Log("=== ScanSwapchain started ===");

    HMODULE hDXGI = GetModuleHandleA("dxgi.dll");
    if (!hDXGI) { Log("dxgi.dll not found"); return; }
    MODULEINFO dxgiInfo;
    GetModuleInformation(GetCurrentProcess(), hDXGI, &dxgiInfo, sizeof(dxgiInfo));
    uintptr_t dxgiBase = (uintptr_t)dxgiInfo.lpBaseOfDll;
    uintptr_t dxgiEnd = dxgiBase + dxgiInfo.SizeOfImage;
    Log("dxgi.dll: %p - %p", (void*)dxgiBase, (void*)dxgiEnd);

    HMODULE hGame = GetModuleHandleA("BorderlandsGOTY.exe");
    if (!hGame) { Log("Game module not found"); return; }
    MODULEINFO gameInfo;
    GetModuleInformation(GetCurrentProcess(), hGame, &gameInfo, sizeof(gameInfo));
    uintptr_t gameBase = (uintptr_t)gameInfo.lpBaseOfDll;
    Log("Game: %p, size: 0x%X", (void*)gameBase, gameInfo.SizeOfImage);

    // Scan game's writable sections
    IMAGE_DOS_HEADER* dos = (IMAGE_DOS_HEADER*)gameBase;
    IMAGE_NT_HEADERS* nt = (IMAGE_NT_HEADERS*)(gameBase + dos->e_lfanew);
    IMAGE_SECTION_HEADER* sections = IMAGE_FIRST_SECTION(nt);

    int found = 0;
    for (int i = 0; i < nt->FileHeader.NumberOfSections; i++) {
        const IMAGE_SECTION_HEADER& sec = sections[i];
        if (!(sec.Characteristics & IMAGE_SCN_MEM_WRITE)) continue;
        uintptr_t secStart = gameBase + sec.VirtualAddress;
        DWORD secSize = sec.Misc.VirtualSize;
        Log("Section %.8s: %p, 0x%X bytes", sec.Name, (void*)secStart, secSize);

        for (uintptr_t addr = secStart; addr + 8 <= secStart + secSize; addr += 8) {
            uintptr_t value = 0;
            if (!ReadProcessMemory(GetCurrentProcess(), (LPCVOID)addr, &value, sizeof(value), nullptr))
                continue;
            if (value < dxgiBase || value >= dxgiEnd || (value & 7)) continue;

            uintptr_t present = 0;
            if (!ReadProcessMemory(GetCurrentProcess(), (LPCVOID)(value + 8*8), &present, sizeof(present), nullptr))
                continue;
            if (present < dxgiBase || present >= dxgiEnd) continue;

            int hits = 0;
            for (int j = 0; j < 25; j++) {
                uintptr_t entry = 0;
                if (ReadProcessMemory(GetCurrentProcess(), (LPCVOID)(value + j*8), &entry, sizeof(entry), nullptr))
                    if (entry >= dxgiBase && entry < dxgiEnd) hits++;
            }

            if (hits >= 20) {
                Log("VTable %p (from %p): Present=%p (%d/25)", (void*)value, (void*)addr, (void*)present, hits);
                found++;
            }
        }
    }

    Log("=== Found %d vtable candidates ===", found);
}

BOOL APIENTRY DllMain(HMODULE, DWORD reason, LPVOID) {
    if (reason == DLL_PROCESS_ATTACH) {
        DisableThreadLibraryCalls(GetModuleHandleA(NULL));
        // Run scan in a new thread to avoid loader lock
        CreateThread(NULL, 0, (LPTHREAD_START_ROUTINE)ScanSwapchain, NULL, 0, NULL);
    }
    return TRUE;
}
