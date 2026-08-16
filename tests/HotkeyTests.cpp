#include "pch.h"
#include <gtest/gtest.h>
#include "EditState.h"

// Define a dummy g_hotkeys if the compiler/linker requires it due to EditState.h extern
std::array<HotkeyBinding, static_cast<size_t>(HotkeyAction::Count)> g_hotkeys = {};

TEST(HotkeyTest, KeyComboIsEmptyFiltersModifiers) {
    // Empty key combo
    EXPECT_TRUE((KeyCombo{ 0, 0 }.IsEmpty()));
    
    // Pure modifiers must be treated as empty
    EXPECT_TRUE((KeyCombo{ VK_CONTROL, 0 }.IsEmpty()));
    EXPECT_TRUE((KeyCombo{ VK_SHIFT, 0 }.IsEmpty()));
    EXPECT_TRUE((KeyCombo{ VK_MENU, 0 }.IsEmpty()));
    EXPECT_TRUE((KeyCombo{ VK_LCONTROL, 1 }.IsEmpty()));
    EXPECT_TRUE((KeyCombo{ VK_RMENU, 4 }.IsEmpty()));
    
    // Standard keys should not be empty
    EXPECT_FALSE((KeyCombo{ 'A', 0 }.IsEmpty()));
    EXPECT_FALSE((KeyCombo{ VK_SPACE, 0 }.IsEmpty()));
    EXPECT_FALSE((KeyCombo{ 'O', 1 }.IsEmpty()));
}

TEST(HotkeyTest, StringToKeyComboParsing) {
    // Valid standard combinations
    EXPECT_EQ(StringToKeyCombo(L"Ctrl+O"), (KeyCombo{ 'O', 1 }));
    EXPECT_EQ(StringToKeyCombo(L"Ctrl+Shift+C"), (KeyCombo{ 'C', 3 }));
    EXPECT_EQ(StringToKeyCombo(L"Ctrl+Alt+C"), (KeyCombo{ 'C', 5 }));
    EXPECT_EQ(StringToKeyCombo(L"Alt+Left"), (KeyCombo{ VK_LEFT, 4 }));
    
    // Named keys
    EXPECT_EQ(StringToKeyCombo(L"Space"), (KeyCombo{ VK_SPACE, 0 }));
    EXPECT_EQ(StringToKeyCombo(L"Esc"), (KeyCombo{ VK_ESCAPE, 0 }));
    EXPECT_EQ(StringToKeyCombo(L"F11"), (KeyCombo{ VK_F11, 0 }));
    
    // Mouse buttons
    EXPECT_EQ(StringToKeyCombo(L"MButton"), (KeyCombo{ VK_MBUTTON, 0 }));
    EXPECT_EQ(StringToKeyCombo(L"Ctrl+XButton1"), (KeyCombo{ VK_XBUTTON1, 1 }));
    EXPECT_EQ(StringToKeyCombo(L"XButton2"), (KeyCombo{ VK_XBUTTON2, 0 }));
    
    // None
    EXPECT_TRUE(StringToKeyCombo(L"").IsEmpty());
    EXPECT_TRUE(StringToKeyCombo(L"None").IsEmpty());
}

TEST(HotkeyTest, KeyComboToStringConversion) {
    // Conversions back to human-readable strings
    EXPECT_EQ(KeyComboToString(KeyCombo{ 'O', 1 }), L"Ctrl+O");
    EXPECT_EQ(KeyComboToString(KeyCombo{ 'C', 3 }), L"Ctrl+Shift+C");
    EXPECT_EQ(KeyComboToString(KeyCombo{ 'C', 5 }), L"Ctrl+Alt+C");
    EXPECT_EQ(KeyComboToString(KeyCombo{ VK_LEFT, 4 }), L"Alt+Left");
    EXPECT_EQ(KeyComboToString(KeyCombo{ VK_SPACE, 0 }), L"Space");
    EXPECT_EQ(KeyComboToString(KeyCombo{ 0, 0 }), L"None");
    
    EXPECT_EQ(KeyComboToString(KeyCombo{ VK_MBUTTON, 0 }), L"MButton");
    EXPECT_EQ(KeyComboToString(KeyCombo{ VK_XBUTTON1, 1 }), L"Ctrl+XButton1");
    EXPECT_EQ(KeyComboToString(KeyCombo{ VK_XBUTTON2, 0 }), L"XButton2");
}

TEST(HotkeyTest, HotkeyActionToStringAndParsing) {
    EXPECT_EQ(HotkeyActionToString(HotkeyAction::CopyPixels), L"CopyPixels");
    EXPECT_EQ(HotkeyActionToString(HotkeyAction::CopyFileItem), L"CopyFile");
    EXPECT_EQ(HotkeyActionToString(HotkeyAction::CopyPath), L"CopyPath");
    EXPECT_EQ(HotkeyActionToString(HotkeyAction::ToggleInfoPanel), L"ToggleInfoPanel");
    EXPECT_EQ(HotkeyActionToString(HotkeyAction::ToggleExifPanel), L"ToggleExifPanel");
    EXPECT_EQ(HotkeyActionToString(HotkeyAction::ToggleGallery), L"ToggleGallery");
    EXPECT_EQ(HotkeyActionToString(HotkeyAction::ToggleCompare), L"ToggleCompare");
    EXPECT_EQ(HotkeyActionToString(HotkeyAction::ComparePair), L"ComparePair");
    EXPECT_EQ(HotkeyActionToString(HotkeyAction::RenderRaw), L"RenderRaw");

    EXPECT_EQ(StringToHotkeyAction(L"CopyPixels"), HotkeyAction::CopyPixels);
    EXPECT_EQ(StringToHotkeyAction(L"CopyImage"), HotkeyAction::CopyPixels); // Backward compatibility
    EXPECT_EQ(StringToHotkeyAction(L"CopyFile"), HotkeyAction::CopyFileItem);
    EXPECT_EQ(StringToHotkeyAction(L"CopyFileItem"), HotkeyAction::CopyFileItem);
    EXPECT_EQ(StringToHotkeyAction(L"CopyPath"), HotkeyAction::CopyPath);
    EXPECT_EQ(StringToHotkeyAction(L"ToggleInfoPanel"), HotkeyAction::ToggleInfoPanel);
    EXPECT_EQ(StringToHotkeyAction(L"ToggleExifPanel"), HotkeyAction::ToggleExifPanel);
    EXPECT_EQ(StringToHotkeyAction(L"ToggleGallery"), HotkeyAction::ToggleGallery);
    EXPECT_EQ(StringToHotkeyAction(L"ToggleCompare"), HotkeyAction::ToggleCompare);
    EXPECT_EQ(StringToHotkeyAction(L"ComparePair"), HotkeyAction::ComparePair);
    EXPECT_EQ(StringToHotkeyAction(L"RenderRaw"), HotkeyAction::RenderRaw);
}


