#pragma once

#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#include <windows.h>
#include <string>

// Run the main Win32 GUI application
int RunGuiApp(HINSTANCE hInstance, bool start_minimized);

// Execute a single headless connection & authentication attempt (for --connect-once CLI mode)
int RunConnectOnceCli();
