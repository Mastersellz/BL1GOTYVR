#include <Windows.h>
#include <TlHelp32.h>
#include <ShlObj.h>
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

    char injectorDirectory[MAX_PATH] = {};
    if (!GetModuleFileNameA(nullptr, injectorDirectory, MAX_PATH)) {
        printf("[Injector] ERROR: GetModuleFileName failed: %lu\n", GetLastError());
        return 1;
    }
    char* directoryEnd = strrchr(injectorDirectory, '\\');
    if (!directoryEnd) {
        printf("[Injector] ERROR: Could not resolve injector directory\n");
        return 1;
    }
    *directoryEnd = '\0';

    char modIni[MAX_PATH] = {};
    sprintf_s(modIni, "%s\\BL1GOTYVR.ini", injectorDirectory);
    const int renderWidth = GetPrivateProfileIntA("Display", "Width", 2048, modIni);
    const int renderHeight = GetPrivateProfileIntA("Display", "Height", 2048, modIni);
    char documents[MAX_PATH] = {};
    if (SUCCEEDED(SHGetFolderPathA(nullptr, CSIDL_PERSONAL, nullptr,
                                   SHGFP_TYPE_CURRENT, documents))) {
        char gameIni[MAX_PATH] = {};
        sprintf_s(gameIni, "%s\\My Games\\Borderlands Game of the Year\\"
                           "WillowGame\\Config\\WillowEngine.ini", documents);
        if (GetFileAttributesA(gameIni) != INVALID_FILE_ATTRIBUTES) {
            char widthText[16] = {};
            char heightText[16] = {};
            sprintf_s(widthText, "%d", renderWidth);
            sprintf_s(heightText, "%d", renderHeight);
            const BOOL widthWritten = WritePrivateProfileStringA(
                "SystemSettings", "ResX", widthText, gameIni);
            const BOOL heightWritten = WritePrivateProfileStringA(
                "SystemSettings", "ResY", heightText, gameIni);
            // Texture streaming must stay ON (disabling it leaves BL1 GOTY
            // textures black). Prevent VR head-turn pop-in instead by growing
            // the streaming pool so mips are not evicted.
            const BOOL streamingWritten = WritePrivateProfileStringA(
                "Engine.Engine", "bUseTextureStreaming", "True", gameIni);
            const BOOL poolWritten = WritePrivateProfileStringA(
                "TextureStreaming", "PoolSize", "8192", gameIni);
            const BOOL dynamicStreamingWritten = WritePrivateProfileStringA(
                "TextureStreaming", "DynamicStreaming", "2", gameIni);
            printf("[Injector] Prepared game resolution %dx%d: %s\n", renderWidth,
                   renderHeight, widthWritten && heightWritten ? "OK" : "FAILED");
            printf("[Injector] Texture streaming pool enlarged (pop-in fix): %s\n",
                   streamingWritten && poolWritten && dynamicStreamingWritten ? "OK" : "FAILED");
        }
    }

    printf("[Injector] Waiting for %s...\n", targetExe);

    DWORD pid = 0;
    while (!pid) {
        pid = FindProcess(targetExe);
        if (!pid) Sleep(1000);
    }

    printf("[Injector] Found %s (PID %lu)\n", targetExe, pid);
    printf("[Injector] Injecting immediately so display hooks precede viewport setup...\n");

    // Resolve the DLL beside this injector, not from the caller's working directory.
    char fullPath[MAX_PATH] = {};
    if (sprintf_s(fullPath, "%s\\BL1GOTYVR.dll", injectorDirectory) < 0) {
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
