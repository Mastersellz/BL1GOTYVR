/**
 * find_present_hwbp.cpp — Hardware breakpoint (Dr0-Dr3) for Present detection
 * Uses debug registers instead of INT3 — doesn't corrupt registers.
 */
#include <windows.h>
#include <cstdio>

static FILE* g_log = NULL;
static LONG g_hitCount = 0;
static LONG g_dr7Original = 0;

static void Log(const char* fmt, ...) {
    char buf[512];
    va_list args;
    va_start(args, fmt);
    vsnprintf(buf, sizeof(buf), fmt, args);
    va_end(args);
    OutputDebugStringA(buf);
    if (g_log) { fprintf(g_log, "%s\n", buf); fflush(g_log); }
}

// Known call sites: mov rax,[reg]; mov rcx,reg; call [rax+40h]
// The CALL instruction (FF 50 40) is at these addresses:
static uintptr_t g_callSites[] = {
    0x7FF722BE9223 + 3,  // +3 to point at FF 50 40
    0x7FF722C9F018 + 3,  // confirmed hot site (65k hits/10s)
    0x7FF722CB322E + 3,
};

static const int NUM_DR = 3;  // Use Dr0, Dr1, Dr2
static BOOL g_inHook = FALSE;

static LONG WINAPI VectoredHandler(EXCEPTION_POINTERS* ep) {
    if (ep->ExceptionRecord->ExceptionCode == EXCEPTION_SINGLE_STEP) {
        CONTEXT* ctx = ep->ContextRecord;

        // Check which hardware breakpoint fired
        DWORD dr6 = ctx->Dr6;
        int hitDr = -1;
        if (dr6 & 0x1) hitDr = 0;
        else if (dr6 & 0x2) hitDr = 1;
        else if (dr6 & 0x4) hitDr = 2;

        if (hitDr >= 0 && hitDr < NUM_DR && !g_inHook) {
            g_inHook = TRUE;
            LONG count = InterlockedIncrement(&g_hitCount);

            uintptr_t rip = ctx->Rip;
            uintptr_t rax = ctx->Rax;
            uintptr_t rbx = ctx->Rbx;
            uintptr_t rcx = ctx->Rcx;
            uintptr_t rdi = ctx->Rdi;
            uintptr_t rsp = ctx->Rsp;

            // Read return address from stack
            uintptr_t retAddr = 0;
            ReadProcessMemory(GetCurrentProcess(), (LPCVOID)rsp, &retAddr, sizeof(retAddr), NULL);

            // Read vtable[8] = Present
            uintptr_t presentAddr = 0;
            if (rax > 0x1000) {
                ReadProcessMemory(GetCurrentProcess(), (LPCVOID)(rax + 8*8), &presentAddr, sizeof(presentAddr), NULL);
            }

            if (count <= 20 || count % 1000 == 0) {
                Log("HIT #%ld Dr%d @ %p", count, hitDr, (void*)rip);
                Log("  RAX=%p RBX=%p RCX=%p RDI=%p", (void*)rax, (void*)rbx, (void*)rcx, (void*)rdi);
                Log("  RSP=%p RetAddr=%p", (void*)rsp, (void*)retAddr);
                Log("  Present(vtable[8])=%p", (void*)presentAddr);
            }

            // Disable this Dr breakpoint temporarily, single-step, re-enable
            DWORD dr7 = ctx->Dr7;
            ctx->Dr7 = dr7 & ~(1 << (hitDr * 2));  // Disable local enable for this Dr
            ctx->EFlags |= 0x100;  // Set TF for single-step

            g_inHook = FALSE;
            return EXCEPTION_CONTINUE_EXECUTION;
        }

        // Single-step after hardware breakpoint — re-enable
        if (ctx->EFlags & 0x100) {
            ctx->EFlags &= ~0x100;  // Clear TF
            // Re-enable all Dr breakpoints
            ctx->Dr7 = g_dr7Original;
            return EXCEPTION_CONTINUE_EXECUTION;
        }
    }

    return EXCEPTION_CONTINUE_SEARCH;
}

static DWORD WINAPI MonitorThread(LPVOID) {
    Log("=== Hardware breakpoint Present monitor started ===");

    // Install VEH (first handler)
    PVOID veh = AddVectoredExceptionHandler(1, VectoredHandler);
    Log("VEH installed: %p", veh);

    // Set hardware breakpoints on call sites
    CONTEXT ctx = {};
    ctx.ContextFlags = CONTEXT_DEBUG_REGISTERS;

    HANDLE hThread = GetCurrentThread();
    GetThreadContext(hThread, &ctx);

    // Save original Dr7
    g_dr7Original = ctx.Dr7;

    for (int i = 0; i < NUM_DR && i < (int)(sizeof(g_callSites)/sizeof(g_callSites[0])); i++) {
        switch (i) {
            case 0: ctx.Dr0 = g_callSites[i]; break;
            case 1: ctx.Dr1 = g_callSites[i]; break;
            case 2: ctx.Dr2 = g_callSites[i]; break;
        }
        Log("Set Dr%d = %p", i, (void*)g_callSites[i]);
    }

    // Enable execution breakpoints: Dr7 local enable bits for Dr0,1,2
    // Bit 0 (L0), Bit 2 (L1), Bit 4 (L2) = local enable
    // Bits 16-17 (R/W0), 20-21 (R/W1), 24-25 (R/W2) = 00 = execution
    // Bits 18-19 (LEN0), 22-23 (LEN1), 26-27 (LEN2) = 00 = 1 byte
    ctx.Dr7 |= (1 << 0) | (1 << 2) | (1 << 4);  // L0, L1, L2
    ctx.Dr7 &= ~((3<<16) | (3<<20) | (3<<24));  // R/W = 00 (exec)
    ctx.Dr7 &= ~((3<<18) | (3<<22) | (3<<26));  // LEN = 00 (1 byte)

    g_dr7Original = ctx.Dr7;

    SetThreadContext(hThread, &ctx);
    Log("Hardware breakpoints enabled on %d call sites", NUM_DR);

    // Monitor loop
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
        g_log = fopen("C:\\Vrengine\\BL1GOTYVR\\present_hwbp.txt", "w");
        CreateThread(NULL, 0, MonitorThread, NULL, 0, NULL);
    }
    else if (reason == DLL_PROCESS_DETACH) {
        if (g_log) { fclose(g_log); g_log = NULL; }
    }
    return TRUE;
}
