#include "gui.h"

#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#include <windows.h>
#include <shellapi.h>

#include <string>

int WINAPI wWinMain(HINSTANCE hInstance, HINSTANCE /*hPrevInstance*/, PWSTR /*pCmdLine*/, int /*nCmdShow*/) {
    bool start_minimized = false;
    bool connect_once = false;

    int argc = 0;
    LPWSTR* argv = CommandLineToArgvW(GetCommandLineW(), &argc);
    if (argv) {
        for (int i = 1; i < argc; ++i) {
            std::wstring arg = argv[i];
            if (arg == L"--minimized" || arg == L"--tray" || arg == L"-m") {
                start_minimized = true;
            } else if (arg == L"--connect-once" || arg == L"-c") {
                connect_once = true;
            }
        }
        LocalFree(argv);
    }

    if (connect_once) {
        return RunConnectOnceCli();
    }

    // Ensure single instance for GUI/Tray mode
    HANDLE hMutex = CreateMutexW(nullptr, TRUE, L"Local\\AutoWiFi_SingleInstance_Mutex");
    if (hMutex && GetLastError() == ERROR_ALREADY_EXISTS) {
        HWND hExisting = FindWindowW(L"AutoWiFiMainWindowClass", nullptr);
        if (hExisting) {
            ShowWindow(hExisting, SW_SHOW);
            ShowWindow(hExisting, SW_RESTORE);
            SetForegroundWindow(hExisting);
        }
        CloseHandle(hMutex);
        return 0;
    }

    int ret = RunGuiApp(hInstance, start_minimized);

    if (hMutex) {
        ReleaseMutex(hMutex);
        CloseHandle(hMutex);
    }
    return ret;
}
