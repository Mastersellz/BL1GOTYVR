/**
 * debug_present.cpp — Detailed debug of Present call site
 * Logs: full context, instruction bytes, stack dump, memory around RAX
 */
#include <windows.h>
#include <cstdio>

static FILE* g_log = NULL;
static LONG g_hitCount = 0;

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
        Log("  %s: %s", label, hex);
    }
}

// We hook at site 1 (the confirmed hot site) using INT3
// But this time we DON'T restore or single-step — just log and let it crash
// The game will recover via SEH or we'll get one good log

static uintptr_t g_site = 0;
static BYTE g_origByte = 0;

static LONG WINAPI VectoredHandler(EXCEPTION_POINTERS* ep) {
    if (ep->ExceptionRecord->ExceptionCode == EXCEPTION_BREAKPOINT) {
        uintptr_t rip = (uintptr_t)ep->ExceptionRecord->ExceptionAddress;
        if (rip == g_site + 3) {  // Our INT3 at offset +3
            LONG count = InterlockedIncrement(&g_hitCount);
            CONTEXT* ctx = ep->ContextRecord;

            if (count <= 10) {
                Log("=== INT3 HIT #%ld ===", count);
                Log("RIP: %p", (void*)ctx->Rip);
                Log("RAX: %p  RBX: %p  RCX: %p  RDX: %p", (void*)ctx->Rax, (void*)ctx->Rbx, (void*)ctx->Rcx, (void*)ctx->Rdx);
                Log("RSI: %p  RDI: %p  RBP: %p  RSP: %p", (void*)ctx->Rsi, (void*)ctx->Rdi, (void*)ctx->Rbp, (void*)ctx->Rsp);
                Log("R8: %p  R9: %p  R10: %p  R11: %p", (void*)ctx->R8, (void*)ctx->R9, (void*)ctx->R10, (void*)ctx->R11);
                Log("R12: %p  R13: %p  R14: %p  R15: %p", (void*)ctx->R12, (void*)ctx->R13, (void*)ctx->R14, (void*)ctx->R15);
                Log("EFlags: 0x%X", ctx->EFlags);

                // Log instruction bytes at RIP
                Log("Instructions at RIP:");
                HexDump("RIP", ctx->Rip, 32);

                // Log instruction bytes at the call site start
                Log("Instructions at call site start:");
                HexDump("Site", g_site, 32);

                // Log stack (first 16 qwords)
                Log("Stack (RSP):");
                HexDump("RSP", ctx->Rsp, 128);

                // Log memory around RAX (vtable?)
                if (ctx->Rax > 0x1000 && ctx->Rax < 0x7FFFFFFFFFFF) {
                    Log("Memory at RAX (vtable?):");
                    HexDump("RAX", ctx->Rax, 128);
                }

                // Log memory around RCX (this pointer?)
                if (ctx->Rcx > 0x1000 && ctx->Rcx < 0x7FFFFFFFFFFF) {
                    Log("Memory at RCX (this?):");
                    HexDump("RCX", ctx->Rcx, 64);
                }

                // Read vtable[8] if RAX looks like a vtable
                if (ctx->Rax > 0x1000 && ctx->Rax < 0x7FFFFFFFFFFF) {
                    uintptr_t vtableEntry8 = 0;
                    if (ReadProcessMemory(GetCurrentProcess(), (LPCVOID)(ctx->Rax + 8*8), &vtableEntry8, sizeof(vtableEntry8), NULL)) {
                        Log("vtable[8] (Present?): %p", (void*)vtableEntry8);
                    }
                }

                // Read return address from stack
                uintptr_t retAddr = 0;
                if (ReadProcessMemory(GetCurrentProcess(), (LPCVOID)ctx->Rsp, &retAddr, sizeof(retAddr), NULL)) {
                    Log("Return address (stack[RSP]): %p", (void*)retAddr);
                }

                Log("=== END HIT #%ld ===", count);
            }

            // Restore original byte and let it execute normally
            DWORD oldProtect;
            VirtualProtect((LPVOID)g_site, 16, PAGE_EXECUTE_READWRITE, &oldProtect);
            WriteProcessMemory(GetCurrentProcess(), (LPVOID)(g_site + 3), &g_origByte, 1, NULL);
            VirtualProtect((LPVOID)g_site, 16, oldProtect, &oldProtect);

            // Don't modify EFlags — let it execute the original instruction
            return EXCEPTION_CONTINUE_EXECUTION;
        }
    }
    return EXCEPTION_CONTINUE_SEARCH;
}

static DWORD WINAPI DebugThread(LPVOID) {
    Log("=== Debug Present monitor started ===");

    // CORRECTED: The CALL [rax+40h] instruction (FF 50 40) is at 0x7FF722C9F01B
    // Previous INT3 was at this address + 3 = 0x7FF722C9F01E (WRONG — that's TEST)
    // Correct: INT3 directly at the FF 50 40 instruction
    g_site = 0x7FF722C9F01B - 3;  // g_site + 3 = 0x7FF722C9F01B = the CALL

    // Read original byte
    ReadProcessMemory(GetCurrentProcess(), (LPCVOID)(g_site + 3), &g_origByte, 1, NULL);
    Log("Original byte at site+3: 0x%02X", g_origByte);

    // Install VEH
    AddVectoredExceptionHandler(1, VectoredHandler);
    Log("VEH installed");

    // Patch with INT3
    DWORD oldProtect;
    VirtualProtect((LPVOID)g_site, 16, PAGE_EXECUTE_READWRITE, &oldProtect);
    BYTE int3 = 0xCC;
    WriteProcessMemory(GetCurrentProcess(), (LPVOID)(g_site + 3), &int3, 1, NULL);
    VirtualProtect((LPVOID)g_site, 16, oldProtect, &oldProtect);
    Log("INT3 patched at %p + 3", (void*)g_site);

    // Wait and log periodically
    while (true) {
        Sleep(3000);
        LONG hits = InterlockedExchange(&g_hitCount, 0);
        if (hits > 0) {
            Log("Monitor: %ld hits in 3s", hits);
        }
    }
    return 0;
}

BOOL APIENTRY DllMain(HMODULE hModule, DWORD reason, LPVOID) {
    if (reason == DLL_PROCESS_ATTACH) {
        DisableThreadLibraryCalls(hModule);
        g_log = fopen("C:\\Vrengine\\BL1GOTYVR\\debug_present.txt", "w");
        CreateThread(NULL, 0, DebugThread, NULL, 0, NULL);
    }
    else if (reason == DLL_PROCESS_DETACH) {
        if (g_log) { fclose(g_log); g_log = NULL; }
    }
    return TRUE;
}
