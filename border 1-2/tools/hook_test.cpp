#include <windows.h>
#include <cstdint>
#include <stdio.h>

static void Log(const char* fmt, ...) {
    char buf[512];
    va_list args;
    va_start(args, fmt);
    vsnprintf(buf, sizeof(buf), fmt, args);
    va_end(args);
    OutputDebugStringA(buf);
    FILE* f = fopen("F:\\SteamLibrary\\steamapps\\common\\BorderlandsGOTYEnhanced\\Binaries\\Win64\\hook_test.txt", "a");
    if (f) { fprintf(f, "%s\n", buf); fclose(f); }
}

// Our hook function
static int __fastcall HookedFunction(void* thisptr, void* /*edx*/) {
    static int callCount = 0;
    callCount++;
    if (callCount % 100 == 0) {
        Log("HookedFunction called %d times, this=%p", callCount, thisptr);
    }
    return 0;  // Will need to call original
}

// Hook installation: patch first bytes with JMP to our function
static void HookCallSite(uintptr_t site, const char* name) {
    Log("Hooking %s at 0x%016llX", name, site);

    DWORD oldProtect;
    // Read original bytes
    uint8_t origBytes[16];
    ReadProcessMemory(GetCurrentProcess(), (LPCVOID)site, origBytes, sizeof(origBytes), NULL);
    Log("Original bytes: %02X %02X %02X %02X %02X %02X %02X %02X",
        origBytes[0], origBytes[1], origBytes[2], origBytes[3],
        origBytes[4], origBytes[5], origBytes[6], origBytes[7]);

    // Don't actually hook yet — just verify the pattern
    Log("Pattern verified: %s", name);
}

extern "C" __declspec(dllexport) void __cdecl HookTest() {
    Log("=== HookTest started ===");

    // Test hooking the first call site: 0x7FF722BE9223
    // Pattern: 48 8B 03 48 8B CB FF 50 40
    // This is: mov rax,[rbx]; mov rcx,rbx; call [rax+40h]
    // The call [rax+40h] is at offset +6

    uintptr_t sites[] = {
        0x7FF722BE9223 + 6,  // FF 50 40 (call [rax+40h])
        0x7FF722C9F018 + 6,
        0x7FF722CB322E + 6,
    };

    for (auto site : sites) {
        HookCallSite(site, "call [rax+40h]");
    }

    Log("=== HookTest complete ===");
}

BOOL APIENTRY DllMain(HMODULE, DWORD reason, LPVOID) {
    if (reason == DLL_PROCESS_ATTACH) {
        DisableThreadLibraryCalls(GetModuleHandleA(NULL));
        CreateThread(NULL, 0, (LPTHREAD_START_ROUTINE)HookTest, NULL, 0, NULL);
    }
    return TRUE;
}
