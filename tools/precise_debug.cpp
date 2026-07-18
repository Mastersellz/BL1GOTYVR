/**
 * precise_debug.cpp — Read exact bytes at candidate Present addresses
 * and try INT3 at each one to find the real call site
 */
#include <windows.h>
#include <cstdio>

static FILE* g_log = NULL;
static LONG g_hitCount = 0;
static int g_sitesTested = 0;

static void Log(const char* fmt, ...) {
    char buf[1024];
    va_list args;
    va_start(args, fmt);
    vsnprintf(buf, sizeof(buf), fmt, args);
    va_end(args);
    OutputDebugStringA(buf);
    if (g_log) { fprintf(g_log, "%s\n", buf); fflush(g_log); }
}

static void HexDump(const char* label, uintptr_t addr, int len) {
    BYTE buf[64];
    if (len > 64) len = 64;
    if (ReadProcessMemory(GetCurrentProcess(), (LPCVOID)addr, buf, len, NULL)) {
        char hex[256] = {};
        for (int i = 0; i < len; i++) sprintf(hex + i*3, "%02X ", buf[i]);
        Log("  %s @ %p: %s", label, (void*)addr, hex);
    } else {
        Log("  %s @ %p: READ FAILED", label, (void*)addr);
    }
}

// Try multiple candidate addresses for the CALL instruction
static uintptr_t g_candidates[] = {
    0x7FF722BE9223, 0x7FF722BE9226, 0x7FF722BE9229,
    0x7FF722C9F015, 0x7FF722C9F018, 0x7FF722C9F01B, 0x7FF722C9F01E,
    0x7FF722CB322B, 0x7FF722CB322E, 0x7FF722CB3231,
    0x7FF722CAD36C, 0x7FF722CAD36F,
    0x7FF722CAFA75, 0x7FF722CAFA78,
    0x7FF722CC27A1, 0x7FF722CC27A4,
    0x7FF722CC291C, 0x7FF722CC291F,
    0x7FF722CC57FC, 0x7FF722CC57FF,
    0x7FF722D0AFF8, 0x7FF722D0AFFB,
    0x7FF722D9A621, 0x7FF722D9A624,
};

static const int NUM_CANDIDATES = sizeof(g_candidates) / sizeof(g_candidates[0]);
static BYTE g_origBytes[128] = {};
static int g_patchedIdx = -1;

static LONG WINAPI VectoredHandler(EXCEPTION_POINTERS* ep) {
    if (ep->ExceptionRecord->ExceptionCode == EXCEPTION_BREAKPOINT) {
        uintptr_t rip = (uintptr_t)ep->ExceptionRecord->ExceptionAddress;

        // Check all patched sites
        for (int i = 0; i < NUM_CANDIDATES; i++) {
            if (rip == g_candidates[i]) {
                LONG count = InterlockedIncrement(&g_hitCount);
                CONTEXT* ctx = ep->ContextRecord;

                if (count <= 5) {
                    Log("=== HIT #%ld @ %p ===", count, (void*)rip);
                    Log("RAX=%p RBX=%p RCX=%p RDI=%p", (void*)ctx->Rax, (void*)ctx->Rbx, (void*)ctx->Rcx, (void*)ctx->Rdi);
                    Log("RSP=%p", (void*)ctx->Rsp);

                    // Read bytes at RIP
                    HexDump("At RIP", ctx->Rip, 16);

                    // Read vtable[8] if RAX looks like a pointer
                    if (ctx->Rax > 0x10000 && ctx->Rax < 0x7FFFFFFFFFFF) {
                        uintptr_t entry8 = 0;
                        if (ReadProcessMemory(GetCurrentProcess(), (LPCVOID)(ctx->Rax + 8*8), &entry8, sizeof(entry8), NULL)) {
                            Log("vtable[8] (Present?): %p", (void*)entry8);
                        }
                    }

                    // Read return address
                    uintptr_t ret = 0;
                    if (ReadProcessMemory(GetCurrentProcess(), (LPCVOID)ctx->Rsp, &ret, sizeof(ret), NULL)) {
                        Log("Return: %p", (void*)ret);
                    }
                }

                // Restore and let it execute
                DWORD oldProt;
                VirtualProtect((LPVOID)g_candidates[i], 16, PAGE_EXECUTE_READWRITE, &oldProt);
                WriteProcessMemory(GetCurrentProcess(), (LPVOID)g_candidates[i], &g_origBytes[i*6], 3, NULL);
                VirtualProtect((LPVOID)g_candidates[i], 16, oldProt, &oldProt);
                return EXCEPTION_CONTINUE_EXECUTION;
            }
        }
    }
    return EXCEPTION_CONTINUE_SEARCH;
}

static DWORD WINAPI DebugThread(LPVOID) {
    Log("=== Precise Present debug started ===");

    // First, dump bytes at all candidates
    Log("--- Candidate bytes ---");
    for (int i = 0; i < NUM_CANDIDATES; i++) {
        HexDump("Candidate", g_candidates[i], 12);
    }

    // Find which candidates have FF 50 40 (call [rax+40h])
    Log("--- Candidates with FF 50 40 ---");
    for (int i = 0; i < NUM_CANDIDATES; i++) {
        BYTE buf[4] = {};
        if (ReadProcessMemory(GetCurrentProcess(), (LPCVOID)g_candidates[i], buf, 4, NULL)) {
            if (buf[0] == 0xFF && buf[1] == 0x50 && buf[2] == 0x40) {
                Log("FOUND FF 50 40 at %p!", (void*)g_candidates[i]);
            }
        }
    }

    // Install VEH
    AddVectoredExceptionHandler(1, VectoredHandler);

    // Patch candidates that have FF 50 40 with INT3
    int patched = 0;
    for (int i = 0; i < NUM_CANDIDATES; i++) {
        BYTE buf[4] = {};
        if (ReadProcessMemory(GetCurrentProcess(), (LPCVOID)g_candidates[i], buf, 4, NULL)) {
            if (buf[0] == 0xFF && buf[1] == 0x50 && buf[2] == 0x40) {
                // Save original 3 bytes
                memcpy(&g_origBytes[i*6], buf, 3);

                // Patch with INT3
                DWORD oldProt;
                VirtualProtect((LPVOID)g_candidates[i], 16, PAGE_EXECUTE_READWRITE, &oldProt);
                BYTE int3 = 0xCC;
                WriteProcessMemory(GetCurrentProcess(), (LPVOID)g_candidates[i], &int3, 1, NULL);
                VirtualProtect((LPVOID)g_candidates[i], 16, oldProt, &oldProt);

                patched++;
                Log("Patched INT3 at %p", (void*)g_candidates[i]);
            }
        }
    }

    Log("Patched %d candidates with INT3", patched);

    // Wait for hits
    while (true) {
        Sleep(5000);
        LONG hits = InterlockedExchange(&g_hitCount, 0);
        if (hits > 0) Log("Monitor: %ld hits in 5s", hits);
    }
    return 0;
}

BOOL APIENTRY DllMain(HMODULE hModule, DWORD reason, LPVOID) {
    if (reason == DLL_PROCESS_ATTACH) {
        DisableThreadLibraryCalls(hModule);
        g_log = fopen("C:\\Vrengine\\BL1GOTYVR\\precise_debug.txt", "w");
        CreateThread(NULL, 0, DebugThread, NULL, 0, NULL);
    }
    else if (reason == DLL_PROCESS_DETACH) {
        if (g_log) { fclose(g_log); g_log = NULL; }
    }
    return TRUE;
}
