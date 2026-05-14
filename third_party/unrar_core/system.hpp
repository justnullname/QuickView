#ifndef _RAR_SYSTEM_
#define _RAR_SYSTEM_

void InitSystemOptions(int SleepTime);
void SetPriority(int Priority);
void SetProcessPriority(int Priority);
void SetThreadPriority(int Priority);
void Shutdown();
bool ShutdownCheckAnother(bool Shutdown);
clock_t MonoClock();
void Wait();

#ifdef _WIN_ALL
bool SetPrivilege(LPCTSTR PrivName);
HMODULE WINAPI LoadSysLibrary(const wchar *Name);
bool IsUserAdmin();
#endif

#ifdef USE_SSE
enum SSE_VERSION {SSE_NONE,SSE_SSE,SSE_SSE2,SSE_SSSE3,SSE_SSE41,SSE_AVX2};
SSE_VERSION GetSSEVersion();
extern SSE_VERSION _SSE_Version;
#endif

#endif
