#include <Windows.h>
#include <TlHelp32.h>
#include <cstdio>
#include <cstring>

static DWORD FindProcess(const char* name) {
    HANDLE snap = CreateToolhelp32Snapshot(TH32CS_SNAPPROCESS, 0);
    if (snap == INVALID_HANDLE_VALUE) return 0;

    PROCESSENTRY32 pe = {};
    pe.dwSize = sizeof(pe);

    if (Process32First(snap, &pe)) {
        do {
            if (_stricmp(pe.szExeFile, name) == 0) {
                CloseHandle(snap);
                return pe.th32ProcessID;
            }
        } while (Process32Next(snap, &pe));
    }
    CloseHandle(snap);
    return 0;
}

int main() {
    const char* targetExe = "BorderlandsGOTY.exe";

    printf("[Injector] Waiting for %s...\n", targetExe);

    DWORD pid = 0;
    while (!pid) {
        pid = FindProcess(targetExe);
        if (!pid) Sleep(1000);
    }

    printf("[Injector] Found %s (PID %lu)\n", targetExe, pid);
    printf("[Injector] Waiting 5 seconds for game to initialize...\n");
    Sleep(5000);

    // Resolve the DLL beside this injector, not from the caller's working directory.
    char fullPath[MAX_PATH] = {};
    if (!GetModuleFileNameA(nullptr, fullPath, MAX_PATH)) {
        printf("[Injector] ERROR: GetModuleFileName failed: %lu\n", GetLastError());
        return 1;
    }
    char* fileName = strrchr(fullPath, '\\');
    if (!fileName || strcpy_s(fileName + 1, MAX_PATH - (fileName + 1 - fullPath),
                              "BL1GOTYVR.dll") != 0) {
        printf("[Injector] ERROR: Could not resolve DLL beside injector\n");
        return 1;
    }
    printf("[Injector] DLL path: %s\n", fullPath);

    // Open process
    HANDLE hProcess = OpenProcess(PROCESS_ALL_ACCESS, FALSE, pid);
    if (!hProcess) {
        printf("[Injector] ERROR: OpenProcess failed: %lu\n", GetLastError());
        return 1;
    }

    // Allocate memory in target process for DLL path
    size_t pathLen = strlen(fullPath) + 1;
    LPVOID remoteMem = VirtualAllocEx(hProcess, nullptr, pathLen, MEM_COMMIT | MEM_RESERVE, PAGE_READWRITE);
    if (!remoteMem) {
        printf("[Injector] ERROR: VirtualAllocEx failed: %lu\n", GetLastError());
        CloseHandle(hProcess);
        return 1;
    }

    // Write DLL path
    if (!WriteProcessMemory(hProcess, remoteMem, fullPath, pathLen, nullptr)) {
        printf("[Injector] ERROR: WriteProcessMemory failed: %lu\n", GetLastError());
        VirtualFreeEx(hProcess, remoteMem, 0, MEM_RELEASE);
        CloseHandle(hProcess);
        return 1;
    }

    // Get LoadLibraryA address
    HMODULE hKernel32 = GetModuleHandleA("kernel32.dll");
    FARPROC pLoadLibrary = GetProcAddress(hKernel32, "LoadLibraryA");

    // Create remote thread to load DLL
    HANDLE hThread = CreateRemoteThread(hProcess, nullptr, 0,
                                         (LPTHREAD_START_ROUTINE)pLoadLibrary,
                                         remoteMem, 0, nullptr);
    if (!hThread) {
        printf("[Injector] ERROR: CreateRemoteThread failed: %lu\n", GetLastError());
        VirtualFreeEx(hProcess, remoteMem, 0, MEM_RELEASE);
        CloseHandle(hProcess);
        return 1;
    }

    printf("[Injector] DLL injected! Waiting for thread to complete...\n");
    WaitForSingleObject(hThread, 10000);

    DWORD exitCode = 0;
    GetExitCodeThread(hThread, &exitCode);
    printf("[Injector] Thread exit code: %lu\n", exitCode);

    // Cleanup
    CloseHandle(hThread);
    VirtualFreeEx(hProcess, remoteMem, 0, MEM_RELEASE);
    CloseHandle(hProcess);

    if (exitCode != 0) {
        printf("[Injector] SUCCESS — BL1GOTYVR.dll loaded into %s\n", targetExe);
    } else {
        printf("[Injector] WARNING — LoadLibraryA returned NULL (DLL may have failed to load)\n");
    }

    return 0;
}
