#include "Common.h"
#include "DockApp.h"

#include <windows.h>

// Single-instance guard: two docks in one taskbar stretch would fight over the
// same pixels and the same timer.
int WINAPI wWinMain(HINSTANCE instance, HINSTANCE, LPWSTR, int) {
    SetProcessDpiAwarenessContext(DPI_AWARENESS_CONTEXT_PER_MONITOR_AWARE_V2);

    HANDLE mutex = CreateMutexW(nullptr, TRUE, L"Local\\Win11Dock.SingleInstance");
    if (mutex && GetLastError() == ERROR_ALREADY_EXISTS) {
        CloseHandle(mutex);
        return 0;
    }

    int result = 1;
    {
        dock::DockApp app;
        if (app.Init(instance)) {
            result = app.Run();
        }
        app.Shutdown();
    }

    if (mutex) CloseHandle(mutex);
    return result;
}
