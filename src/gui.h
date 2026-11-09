#pragma once

#ifdef _WIN32
#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#include <windows.h>
#endif

#include <string>

#ifdef _WIN32
// Run the main Win32 GUI application
int RunGuiApp(HINSTANCE hInstance, bool start_minimized);
#else
// Run the POSIX (macOS / Linux) interactive TUI or CLI / Daemon mode
int RunPosixApp(int argc, char* argv[]);
#endif

// Execute a single headless connection & authentication attempt (for --connect-once CLI mode)
int RunConnectOnceCli();
