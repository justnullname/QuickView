#include <iostream>
#include <windows.h>
#include <string>

bool IsWindows11() {
    HMODULE hMod = GetModuleHandleW(L"ntdll.dll");
    if (hMod) {
        typedef LONG (WINAPI *RtlGetVersionPtr)(PRTL_OSVERSIONINFOW);
        RtlGetVersionPtr fx = (RtlGetVersionPtr)GetProcAddress(hMod, "RtlGetVersion");
        if (fx) {
            RTL_OSVERSIONINFOW rovi = { 0 };
            rovi.dwOSVersionInfoSize = sizeof(rovi);
            if (fx(&rovi) == 0) {
                if (rovi.dwMajorVersion == 10 && rovi.dwBuildNumber >= 22000) return true;
            }
        }
    }
    return false;
}
int main() { return 0; }
