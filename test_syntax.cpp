struct HWND__ {}; typedef HWND__* HWND;
#define FALSE 0
#define TRUE 1

#include <vector>
#include <string>

bool IsZoomed(HWND h) { return false; }
void InvalidateRect(HWND h, void* r, int e) {}

enum class OptionType { Segment };
struct SettingsItem {
    const wchar_t* label;
    OptionType type;
    void* p1; void* p2;
    int* pIntVal;
    void* p3; int i1; int i2;
    std::vector<const wchar_t*> options;
    void (*onChange)();
};

bool g_isFullScreen = false;
void ApplyFullScreenZoomMode(HWND hwnd) {}

int main() {
    int val = 0;
    SettingsItem itemFsZoom = { L"Label", OptionType::Segment, nullptr, nullptr, &val, nullptr, 0, 0, {L"A", L"B"} };
    itemFsZoom.onChange = []() {
        HWND hwnd = nullptr;
        extern bool g_isFullScreen;
        if (hwnd && (IsZoomed(hwnd) || g_isFullScreen)) {
            extern void ApplyFullScreenZoomMode(HWND hwnd);
            ApplyFullScreenZoomMode(hwnd);
            InvalidateRect(hwnd, nullptr, FALSE);
        }
    };
    return 0;
}
