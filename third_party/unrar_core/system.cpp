#include "rar.hpp"

void InitSystemOptions(int SleepTime) {}
void SetPriority(int Priority) {}
void SetProcessPriority(int Priority) {}
void SetThreadPriority(int Priority) {}
void Shutdown() {}
bool ShutdownCheckAnother(bool Shutdown) { return false; }
clock_t MonoClock() { return clock(); }
void Wait() {}

#ifdef _WIN_ALL
bool SetPrivilege(LPCTSTR PrivName) { return false; }
HMODULE WINAPI LoadSysLibrary(const wchar *Name) { return NULL; }
bool IsUserAdmin() { return false; }
#endif

#ifdef USE_SSE
SSE_VERSION _SSE_Version = SSE_NONE;
SSE_VERSION GetSSEVersion() { return _SSE_Version; }
#endif
