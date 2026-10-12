/**
 * find_present_hwbp2.cpp — Hardware breakpoints on ALL threads
 */
#include <windows.h>
#include <cstdio>
#include <tlhelp32.h>

static FILE* g_log = NULL;
static LONG g_hitCount = 0;
static LONG g_dr7Value = 0;

static void Log(const char* fmt, ...) {
    char buf[512];
    va_list args;
    va_start(args, fmt);
    vsnprintf(buf, sizeof(buf), fmt, args);
    va_end(args);
    OutputDebugStringA(buf);
    if (g_log) { fprintf(g_log, "%s\n", buf); fflush(g_log); }
}

static uintptr_t g_callSites[] = {
    0x7FF722BE9226,  // FF 50 40 at site 0
    0x7FF722C9F01B,  // FF 50 40 at site 1 (confirmed hot)
    0x7FF722CB3231,  // FF 50 40 at site 2
};

static LONG WINAPI VectoredHandler(EXCEPTION_POINTERS* ep) {
    if (ep->ExceptionRecord->ExceptionCode == EXCEPTION_SINGLE_STEP) {
        CONTEXT* ctx = ep->ContextRecord;
        DWORD dr6 = ctx->Dr6;

        int hitDr = -1;
        if (dr6 & 0x1) hitDr = 0;
        else if (dr6 & 0x2) hitDr = 1;
        else if (dr6 & 0x4) hitDr = 2;

        if (hitDr >= 0) {
            LONG count = InterlockedIncrement(&g_hitCount);
            uintptr_t rip = ctx->Rip;
            uintptr_t rax = ctx->Rax;
            uintptr_t rbx = ctx->Rbx;
            uintptr_t rcx = ctx->Rcx;
            uintptr_t rdi = ctx->Rdi;
            uintptr_t rsp = ctx->Rsp;

            uintptr_t retAddr = 0;
            ReadProcessMemory(GetCurrentProcess(), (LPCVOID)rsp, &retAddr, sizeof(retAddr), NULL);

            uintptr_t presentAddr = 0;
            if (rax > 0x1000) {
                ReadProcessMemory(GetCurrentProcess(), (LPCVOID)(rax + 8*8), &presentAddr, sizeof(presentAddr), NULL);
            }

            if (count <= 30 || count % 5000 == 0) {
                Log("HIT #%ld Dr%d @ %p", count, hitDr, (void*)rip);
                Log("  RAX=%p RBX=%p RCX=%p RDI=%p", (void*)rax, (void*)rbx, (void*)rcx, (void*)rdi);
                Log("  RSP=%p RetAddr=%p", (void*)rsp, (void*)retAddr);
                Log("  Present(vtable[8])=%p", (void*)presentAddr);
            }

            // Disable Dr breakpoint, single-step, re-enable
            ctx->Dr7 &= ~(1 << (hitDr * 2));
            ctx->EFlags |= 0x100;
            return EXCEPTION_CONTINUE_EXECUTION;
        }

        // Re-enable after single-step
        if (ctx->EFlags & 0x100) {
            ctx->EFlags &= ~0x100;
            ctx->Dr7 = g_dr7Value;
            return EXCEPTION_CONTINUE_EXECUTION;
        }
    }
    return EXCEPTION_CONTINUE_SEARCH;
}

static void SetHardwareBreakpointsOnAllThreads() {
    DWORD myTid = GetCurrentThreadId();
    DWORD myPid = GetCurrentProcessId();

    HANDLE snap = CreateToolhelp32Snapshot(TH32CS_SNAPTHREAD, 0);
    if (snap == INVALID_HANDLE_VALUE) return;

    THREADENTRY32 te = {};
    te.dwSize = sizeof(te);

    int threadCount = 0;
    int bpSetCount = 0;

    if (Thread32First(snap, &te)) {
        do {
            if (te.th32OwnerProcessID != myPid) continue;
            if (te.th32ThreadID == myTid) continue;  // Skip our thread

            HANDLE hThread = OpenThread(THREAD_GET_CONTEXT | THREAD_SET_CONTEXT, FALSE, te.th32ThreadID);
            if (!hThread) continue;

            threadCount++;

            CONTEXT ctx = {};
            ctx.ContextFlags = CONTEXT_DEBUG_REGISTERS;
            if (GetThreadContext(hThread, &ctx)) {
                // Set Dr0, Dr1, Dr2
                ctx.Dr0 = g_callSites[0];
                ctx.Dr1 = g_callSites[1];
                ctx.Dr2 = g_callSites[2];

                // Enable execution breakpoints
                ctx.Dr7 |= (1 << 0) | (1 << 2) | (1 << 4);  // L0, L1, L2
                ctx.Dr7 &= ~((3<<16) | (3<<20) | (3<<24));  // R/W = exec
                ctx.Dr7 &= ~((3<<18) | (3<<22) | (3<<26));  // LEN = 1 byte

                g_dr7Value = ctx.Dr7;

                if (SetThreadContext(hThread, &ctx)) {
                    bpSetCount++;
                    Log("Set HW BP on thread %d (TID %d)", threadCount, te.th32ThreadID);
                }
            }
            CloseHandle(hThread);
        } while (Thread32Next(snap, &te));
    }
    CloseHandle(snap);

    Log("Set hardware breakpoints on %d/%d threads", bpSetCount, threadCount);
}

static DWORD WINAPI MonitorThread(LPVOID) {
    Log("=== HW BP v2 monitor started ===");

    AddVectoredExceptionHandler(1, VectoredHandler);
    Log("VEH installed");

    // Set breakpoints on all OTHER threads first
    SetHardwareBreakpointsOnAllThreads();

    // Also set on our thread (for completeness)
    CONTEXT ctx = {};
    ctx.ContextFlags = CONTEXT_DEBUG_REGISTERS;
    GetThreadContext(GetCurrentThread(), &ctx);
    ctx.Dr0 = g_callSites[0];
    ctx.Dr1 = g_callSites[1];
    ctx.Dr2 = g_callSites[2];
    ctx.Dr7 |= (1 << 0) | (1 << 2) | (1 << 4);
    ctx.Dr7 &= ~((3<<16) | (3<<20) | (3<<24));
    ctx.Dr7 &= ~((3<<18) | (3<<22) | (3<<26));
    g_dr7Value = ctx.Dr7;
    SetThreadContext(GetCurrentThread(), &ctx);
    Log("Set HW BP on monitor thread too");

    // Monitor
    while (true) {
        Sleep(5000);
        LONG hits = InterlockedExchange(&g_hitCount, 0);
        if (hits > 0) {
            Log("Monitor: %ld hits in 5s", hits);
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
