#pragma once

#include <windows.h>
#include <d2d1_2.h>
#include <optional>
#include <string>
#include <vector>
#include "AppContext.h"

class DialogController {
public:
    explicit DialogController(AppContext& context);
    ~DialogController() = default;

    std::optional<LRESULT> HandleMessage(HWND hwnd, UINT message, WPARAM wParam, LPARAM lParam);
    void Render(ID2D1DeviceContext* ctx);

    DialogResult ShowDialog(HWND hwnd, const std::wstring& title, const std::wstring& messageContent,
                            D2D1_COLOR_F accentColor, const std::vector<DialogButton>& buttons,
                            bool hasCheckbox = false, const std::wstring& checkboxText = L"", const std::wstring& qualityText = L"");

    DialogResult ShowChoiceDialog(HWND hwnd, const std::wstring& title, const std::wstring& messageContent,
                                  D2D1_COLOR_F accentColor, const std::vector<std::wstring>& options,
                                  int& inOutSelectedIndex, const std::vector<DialogButton>& buttons,
                                  bool hasCheckbox = false, const std::wstring& checkboxText = L"", bool isCheckedDefault = false,
                                  bool hasCheckbox2 = false, const std::wstring& checkbox2Text = L"", bool isChecked2Default = false);

    std::wstring ShowInputDialog(HWND hwnd, const std::wstring& title, const std::wstring& message, const std::wstring& initialText, const std::wstring& confirmButtonText = L"");
    std::wstring ShowInputDialog(HWND hwnd, const std::wstring& title, const std::wstring& message, const std::wstring& initialText, const std::vector<DialogButton>& buttons, DialogResult& outResult);

    bool RenderComposite(HWND hwnd);
    void MarkDirty();
    bool IsActive() const;

private:
    AppContext& m_context;
    HWND m_hwnd = nullptr;

    std::optional<LRESULT> OnLButtonDown(HWND hwnd, int x, int y);
    std::optional<LRESULT> OnLButtonUp(HWND hwnd, int x, int y);
    std::optional<LRESULT> OnMouseMove(HWND hwnd, int x, int y);
    std::optional<LRESULT> OnKeyDown(HWND hwnd, WPARAM key);
};
