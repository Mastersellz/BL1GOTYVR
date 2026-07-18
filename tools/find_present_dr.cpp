/**
 * find_present_dr.cpp — Hardware breakpoint debugger for Present
 *
 * Strategy:
 * 1. Create temp device+swapchain (may fail — that's OK)
 * 2. If that fails, hook the game's known Present call sites with INT3
 * 3. When INT3 fires, log the return address
 * 4. The return address tells us which function called Present
 */
#include <windows.h>
#include <cstdio>
#include <signal.h>

static FILE* g_log = NULL;

static void Log(const char* fmt, ...) {
    char buf[512];
    va_list args;
    va_start(args, fmt);
    vsnprintf(buf, sizeof(buf), fmt, args);
    va_end(args);
    OutputDebugStringA(buf);
    if (g_log) { fprintf(g_log, "%s\n", buf); fflush(g_log); }
}

// Known call sites from code scan (FF 50 40 = call [rax+40h])
static uintptr_t g_callSites[] = {
    0x7FF722BE9223,  // mov rcx,rbx; call [rax+40h]
    0x7FF722C9F018,  // mov rcx,rdi; call [rax+40h]
    0x7FF722CB322E,  // another candidate
    0x7FF722CAD36F,
    0x7FF722CAFA78,
    0x7FF722CC27A4,
    0x7FF722CC291F,
    0x7FF722CC57FF,
    0x7FF722D0AFFB,
    0x7FF722D9A624,
};

static const int NUM_SITES = sizeof(g_callSites) / sizeof(g_callSites[0]);
static BYTE g_origBytes[10][16] = {};  // Save original bytes
static LONG g_hitCount = 0;

// VEH handler — catches INT3 at our patched sites
static LONG WINAPI VectoredHandler(EXCEPTION_POINTERS* ep) {
    if (ep->ExceptionRecord->ExceptionCode == EXCEPTION_BREAKPOINT) {
        uintptr_t rip = (uintptr_t)ep->ExceptionRecord->ExceptionAddress;
        // Check if this is one of our patched sites
        for (int i = 0; i < NUM_SITES; i++) {
            if (rip == (uintptr_t)(g_callSites[i] + 3)) {  // +3 because INT3 is at the FF 50 40 offset
                LONG count = InterlockedIncrement(&g_hitCount);
                uintptr_t retAddr = 0;
                // x64: return address is on the stack at [rsp]
                retAddr = ep->ContextRecord->Rsp;
                // Read return address from stack
                ReadProcessMemory(GetCurrentProcess(), (LPCVOID)ep->ContextRecord->Rsp, &retAddr, sizeof(retAddr), NULL);

                // Read rax (vtable pointer)
                uintptr_t rax = ep->ContextRecord->Rax;
                uintptr_t presentAddr = 0;
                ReadProcessMemory(GetCurrentProcess(), (LPCVOID)(rax + 8*8), &presentAddr, sizeof(presentAddr), NULL);

                Log("INT3 HIT #%ld at %p", count, (void*)rip);
                Log("  Return address: %p", (void*)retAddr);
                Log("  RAX (vtable): %p", (void*)rax);
                Log("  Present (vtable[8]): %p", (void*)presentAddr);
                Log("  RBX: %p  RCX: %p  RDI: %p",
                    (void*)ep->ContextRecord->Rbx,
                    (void*)ep->ContextRecord->Rcx,
                    (void*)ep->ContextRecord->Rdi);

                // Restore original byte and single-step
                DWORD oldProtect;
                VirtualProtect((LPVOID)g_callSites[i], 16, PAGE_EXECUTE_READWRITE, &oldProtect);
                WriteProcessMemory(GetCurrentProcess(), (LPVOID)g_callSites[i], g_origBytes[i], 3, NULL);
                VirtualProtect((LPVOID)g_callSites[i], 16, oldProtect, &oldProtect);

                // Set trap flag for single-step
                ep->ContextRecord->EFlags |= 0x100;

                return EXCEPTION_CONTINUE_EXECUTION;
            }
        }
    }
    else if (ep->ExceptionRecord->ExceptionCode == EXCEPTION_SINGLE_STEP) {
        // Re-patch after single-step
        for (int i = 0; i < NUM_SITES; i++) {
            uintptr_t site = g_callSites[i];
            // Check if we're back at one of our sites
            if (ep->ContextRecord->Rip >= site && ep->ContextRecord->Rip < site + 10) {
                DWORD oldProtect;
                VirtualProtect((LPVOID)site, 16, PAGE_EXECUTE_READWRITE, &oldProtect);
                // Write INT3 (0xCC) at the call site
                BYTE int3 = 0xCC;
                WriteProcessMemory(GetCurrentProcess(), (LPVOID)(site + 3), &int3, 1, NULL);  // +3 = FF 50 40 offset
                VirtualProtect((LPVOID)site, 16, oldProtect, &oldProtect);
                break;
            }
        }
        ep->ContextRecord->EFlags &= ~0x100;  // Clear trap flag
        return EXCEPTION_CONTINUE_EXECUTION;
    }

    return EXCEPTION_CONTINUE_SEARCH;
}

static DWORD WINAPI MonitorThread(LPVOID) {
    Log("=== Present call monitor started ===");
    Log("Monitoring %d call sites for Present calls", NUM_SITES);

    // Install VEH
    AddVectoredExceptionHandler(1, VectoredHandler);

    // Patch each call site with INT3
    for (int i = 0; i < NUM_SITES; i++) {
        uintptr_t site = g_callSites[i];
        DWORD oldProtect;

        // Save original bytes
        ReadProcessMemory(GetCurrentProcess(), (LPCVOID)site, g_origBytes[i], 16, NULL);

        // Patch with INT3 at the FF 50 40 position (offset +3)
        VirtualProtect((LPVOID)site, 16, PAGE_EXECUTE_READWRITE, &oldProtect);
        BYTE int3 = 0xCC;
        WriteProcessMemory(GetCurrentProcess(), (LPVOID)(site + 3), &int3, 1, NULL);
        VirtualProtect((LPVOID)site, 16, oldProtect, &oldProtect);

        Log("Patched site %d at 0x%016llX (INT3 at +3)", i, site);
    }

    // Monitor loop — log hit count periodically
    while (true) {
        Sleep(5000);
        LONG hits = InterlockedExchange(&g_hitCount, 0);
        if (hits > 0) {
            Log("Monitor: %ld Present calls in last 5 seconds", hits);
        }
    }

    return 0;
}

BOOL APIENTRY DllMain(HMODULE hModule, DWORD reason, LPVOID) {
    if (reason == DLL_PROCESS_ATTACH) {
        DisableThreadLibraryCalls(hModule);

        // Open log file
        g_log = fopen("F:\\SteamLibrary\\steamapps\\common\\BorderlandsGOTYEnhanced\\Binaries\\Win64\\present_monitor.txt", "w");
        if (!g_log) {
            // Try alternate path
            g_log = fopen("C:\\Vrengine\\BL1GOTYVR\\present_monitor.txt", "w");
        }

        CreateThread(NULL, 0, MonitorThread, NULL, 0, NULL);
    }
    else if (reason == DLL_PROCESS_DETACH) {
        if (g_log) { fclose(g_log); g_log = NULL; }
    }
    return TRUE;
}
