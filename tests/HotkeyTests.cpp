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
    EXPECT_EQ(HotkeyActionToString(HotkeyAction::ToggleFilmstrip), L"ToggleFilmstrip");
    EXPECT_EQ(HotkeyActionToString(HotkeyAction::ToggleCompare), L"ToggleCompare");
    EXPECT_EQ(HotkeyActionToString(HotkeyAction::ComparePair), L"ComparePair");
    EXPECT_EQ(HotkeyActionToString(HotkeyAction::RenderRaw), L"RenderRaw");
    EXPECT_EQ(HotkeyActionToString(HotkeyAction::ToggleSettings), L"ToggleSettings");

    EXPECT_EQ(StringToHotkeyAction(L"CopyPixels"), HotkeyAction::CopyPixels);
    EXPECT_EQ(StringToHotkeyAction(L"CopyImage"), HotkeyAction::CopyPixels); // Backward compatibility
    EXPECT_EQ(StringToHotkeyAction(L"CopyFile"), HotkeyAction::CopyFileItem);
    EXPECT_EQ(StringToHotkeyAction(L"CopyFileItem"), HotkeyAction::CopyFileItem);
    EXPECT_EQ(StringToHotkeyAction(L"CopyPath"), HotkeyAction::CopyPath);
    EXPECT_EQ(StringToHotkeyAction(L"ToggleInfoPanel"), HotkeyAction::ToggleInfoPanel);
    EXPECT_EQ(StringToHotkeyAction(L"ToggleExifPanel"), HotkeyAction::ToggleExifPanel);
    EXPECT_EQ(StringToHotkeyAction(L"ToggleGallery"), HotkeyAction::ToggleGallery);
    EXPECT_EQ(StringToHotkeyAction(L"ToggleFilmstrip"), HotkeyAction::ToggleFilmstrip);
    EXPECT_EQ(StringToHotkeyAction(L"ToggleCompare"), HotkeyAction::ToggleCompare);
    EXPECT_EQ(StringToHotkeyAction(L"ComparePair"), HotkeyAction::ComparePair);
    EXPECT_EQ(StringToHotkeyAction(L"RenderRaw"), HotkeyAction::RenderRaw);
    EXPECT_EQ(StringToHotkeyAction(L"ToggleSettings"), HotkeyAction::ToggleSettings);
    EXPECT_EQ(StringToHotkeyAction(L"Settings"), HotkeyAction::ToggleSettings);
}

TEST(MouseGestureTest, DefaultPresetRulesMatchQuickViewSignature) {
    auto defaultGestures = GetDefaultMouseGestures();
    EXPECT_FALSE(defaultGestures.empty());

    AppConfig config;
    config.MouseGestures = defaultGestures;

    // QuickView signature: None + LeftDrag -> WindowDrag
    EXPECT_EQ(config.GetMouseGestureAction(0, MouseTrigger::LeftDrag), MouseActionTarget::WindowDrag);

    // Default: None + MiddleDrag -> PanImage
    EXPECT_EQ(config.GetMouseGestureAction(0, MouseTrigger::MiddleDrag), MouseActionTarget::PanImage);

    // QuickView signature: None + MiddleClick -> ExitApp
    EXPECT_EQ(config.GetMouseGestureAction(0, MouseTrigger::MiddleClick), MouseActionTarget::ExitApp);

    // QuickView signature: None + RightDrag -> Zoom
    EXPECT_EQ(config.GetMouseGestureAction(0, MouseTrigger::RightDrag), MouseActionTarget::Zoom);
    EXPECT_EQ(config.GetMouseGestureAction(0, MouseTrigger::RightDrag), MouseActionTarget::ZoomDrag);

    // Default modifier: Ctrl + LeftDrag -> CropRegionDrag
    uint8_t ctrlMod = static_cast<uint8_t>(MouseModifier::Ctrl);
    EXPECT_EQ(config.GetMouseGestureAction(ctrlMod, MouseTrigger::LeftDrag), MouseActionTarget::CropRegionDrag);

    // Ctrl + Wheel -> LockWindowZoomWheel
    EXPECT_EQ(config.GetMouseGestureAction(ctrlMod, MouseTrigger::VerticalWheel), MouseActionTarget::LockWindowZoomWheel);
}

TEST(MouseGestureTest, ModifiersBitmaskConversion) {
    constexpr uint8_t ctrlMask = 1;  // Ctrl bitmask
    constexpr uint8_t shiftMask = 2; // Shift bitmask
    constexpr uint8_t altMask = 4;   // Alt bitmask

    uint8_t m1 = 0, m2 = 0;
    MouseGestureRule::ModifiersToMods(ctrlMask | altMask, m1, m2);
    EXPECT_EQ(m1, static_cast<uint8_t>(MouseModifier::Ctrl));
    EXPECT_EQ(m2, static_cast<uint8_t>(MouseModifier::Alt));

    MouseGestureRule rule;
    rule.mod1 = m1;
    rule.mod2 = m2;
    EXPECT_EQ(rule.GetModifiers(), ctrlMask | altMask);

    MouseGestureRule::ModifiersToMods(shiftMask, m1, m2);
    EXPECT_EQ(m1, static_cast<uint8_t>(MouseModifier::Shift));
    EXPECT_EQ(m2, static_cast<uint8_t>(MouseModifier::None));
}

TEST(MouseGestureTest, PresetsSwitching) {
    AppConfig config;
    // Preset 1: Standard viewer mode (LeftDrag = PanImage, LeftDblClick = ToggleFullscreen)
    config.LoadMouseGesturePreset(1);
    EXPECT_EQ(config.GetMouseGestureAction(0, MouseTrigger::LeftDrag), MouseActionTarget::PanImage);
    EXPECT_EQ(config.GetMouseGestureAction(0, MouseTrigger::LeftDblClick), MouseActionTarget::ToggleFullscreen);

    // Preset 0: QuickView signature mode
    config.LoadMouseGesturePreset(0);
    EXPECT_EQ(config.GetMouseGestureAction(0, MouseTrigger::LeftDrag), MouseActionTarget::WindowDrag);
    EXPECT_EQ(config.GetMouseGestureAction(0, MouseTrigger::MiddleClick), MouseActionTarget::ExitApp);
}

TEST(HotkeyRulesTest, MultipleCombosPerAction) {
    AppConfig config;
    // Default rules should contain multiple exit combos (Esc, Ctrl+W, Alt+F4, MButton)
    auto exitCombos = config.GetCombosForAction(HotkeyAction::Exit);
    EXPECT_GE(exitCombos.size(), 4u);

    bool hasEsc = false, hasCtrlW = false, hasAltF4 = false, hasMButton = false;
    for (const auto& cb : exitCombos) {
        if (cb.virtualKey == VK_ESCAPE && cb.modifiers == 0) hasEsc = true;
        if (cb.virtualKey == 'W' && cb.modifiers == 1) hasCtrlW = true;
        if (cb.virtualKey == VK_F4 && cb.modifiers == 4) hasAltF4 = true;
        if (cb.virtualKey == VK_MBUTTON && cb.modifiers == 0) hasMButton = true;
    }
    EXPECT_TRUE(hasEsc);
    EXPECT_TRUE(hasCtrlW);
    EXPECT_TRUE(hasAltF4);
    EXPECT_TRUE(hasMButton);

    // Navigation next should have both D and Right arrow
    auto nextCombos = config.GetCombosForAction(HotkeyAction::NavNext);
    EXPECT_GE(nextCombos.size(), 2u);

    // Test add rule
    KeyCombo customCombo = { 'X', 1 /* Ctrl */ };
    config.AddHotkeyRule(HotkeyAction::Exit, customCombo);
    auto updatedExitCombos = config.GetCombosForAction(HotkeyAction::Exit);
    EXPECT_EQ(updatedExitCombos.size(), exitCombos.size() + 1);

    // Adding duplicate combo should not create duplicate
    config.AddHotkeyRule(HotkeyAction::Exit, customCombo);
    EXPECT_EQ(config.GetCombosForAction(HotkeyAction::Exit).size(), exitCombos.size() + 1);

    // Test remove rule
    config.RemoveHotkeyRule(HotkeyAction::Exit, customCombo);
    EXPECT_EQ(config.GetCombosForAction(HotkeyAction::Exit).size(), exitCombos.size());
}

TEST(HotkeyRulesTest, ConflictDetectionPromptBeforeReassignment) {
    AppConfig config;
    // An action can have multiple hotkeys, but one hotkey combo cannot be bound to multiple actions
    // When a key combo is already taken, it must be detected as conflict (prompting user), NOT silently preempted!
    KeyCombo sharedKey = { 'K', 1 /* Ctrl+K */ };

    // Initially bind Ctrl+K to ToggleFullscreen
    config.AddHotkeyRule(HotkeyAction::ToggleFullscreen, sharedKey);
    auto fsCombos = config.GetCombosForAction(HotkeyAction::ToggleFullscreen);
    bool fsHasShared = false;
    for (const auto& c : fsCombos) {
        if (c == sharedKey) fsHasShared = true;
    }
    EXPECT_TRUE(fsHasShared);

    // Simulate conflict check (as in SettingsOverlay::OnHotkeyCaptured)
    HotkeyAction conflictAction = HotkeyAction::None;
    for (const auto& rule : config.HotkeyRules) {
        if (rule.action != HotkeyAction::ToggleGallery && !rule.combo.IsEmpty() && rule.combo == sharedKey) {
            conflictAction = rule.action;
            break;
        }
    }
    // Conflict is detected! It MUST identify ToggleFullscreen as occupying this key
    EXPECT_EQ(conflictAction, HotkeyAction::ToggleFullscreen);

    // User chooses to cancel -> No reassignment occurs, ToggleFullscreen retains the key
    // User chooses to continue -> Explicitly remove from old action and add to new action
    config.RemoveHotkeyRule(conflictAction, sharedKey);
    config.AddHotkeyRule(HotkeyAction::ToggleGallery, sharedKey);

    auto galleryCombos = config.GetCombosForAction(HotkeyAction::ToggleGallery);
    bool galleryHasShared = false;
    for (const auto& c : galleryCombos) {
        if (c == sharedKey) galleryHasShared = true;
    }
    EXPECT_TRUE(galleryHasShared);

    // Confirm ToggleFullscreen NO LONGER contains Ctrl+K
    fsCombos = config.GetCombosForAction(HotkeyAction::ToggleFullscreen);
    fsHasShared = false;
    for (const auto& c : fsCombos) {
        if (c == sharedKey) fsHasShared = true;
    }
    EXPECT_FALSE(fsHasShared);
}

TEST(MouseGestureTest, OneActionCanDefineMultipleGesturesAndAllButtonsDraggable) {
    AppConfig config;
    // A single action (e.g. PanImage) can be triggered by multiple gestures simultaneously
    config.SetMouseGesture(0, 0, MouseTrigger::MiddleDrag, MouseActionTarget::PanImage);
    config.SetMouseGesture(1, 0, MouseTrigger::LeftDrag, MouseActionTarget::PanImage);   // Ctrl + LeftDrag
    config.SetMouseGesture(2, 0, MouseTrigger::RightDrag, MouseActionTarget::PanImage);  // Shift + RightDrag

    auto panGestures = config.GetGesturesForAction(MouseActionTarget::PanImage);
    EXPECT_GE(panGestures.size(), 3u);

    // All mouse buttons can be configured with dragging actions
    EXPECT_EQ(config.GetMouseGestureAction(0, MouseTrigger::MiddleDrag), MouseActionTarget::PanImage);
    EXPECT_EQ(config.GetMouseGestureAction(1, MouseTrigger::LeftDrag), MouseActionTarget::PanImage);
    EXPECT_EQ(config.GetMouseGestureAction(2, MouseTrigger::RightDrag), MouseActionTarget::PanImage);

    // Right drag can be customized to any action (e.g. Loupe or PanImage)
    config.SetMouseGesture(0, 0, MouseTrigger::RightDrag, MouseActionTarget::Loupe);
    EXPECT_EQ(config.GetMouseGestureAction(0, MouseTrigger::RightDrag), MouseActionTarget::Loupe);

    // Explicit None configuration is accurately recorded
    config.SetMouseGesture(0, 0, MouseTrigger::RightClick, MouseActionTarget::None);
    const auto* rClick = config.FindMouseGesture(0, MouseTrigger::RightClick);
    ASSERT_NE(rClick, nullptr);
    EXPECT_EQ(static_cast<MouseActionTarget>(rClick->action), MouseActionTarget::None);
}

TEST(MouseGestureTest, AltBitmaskAndRoundTrip) {
    EXPECT_EQ(MouseModMask::Ctrl, 1u);
    EXPECT_EQ(MouseModMask::Shift, 2u);
    EXPECT_EQ(MouseModMask::Alt, 4u);
    EXPECT_NE(MouseModMask::Alt, static_cast<uint8_t>(MouseModifier::Alt));

    MouseGestureRule altRule;
    altRule.mod1 = static_cast<uint8_t>(MouseModifier::Alt);
    altRule.mod2 = 0;
    EXPECT_EQ(altRule.GetModifiers(), MouseModMask::Alt);
    EXPECT_NE(altRule.GetModifiers(), MouseModMask::Ctrl | MouseModMask::Shift);

    uint8_t m1 = 0, m2 = 0;
    MouseGestureRule::ModifiersToMods(MouseModMask::Ctrl | MouseModMask::Shift, m1, m2);
    MouseGestureRule ctrlShiftRule{ m1, m2, static_cast<uint8_t>(MouseTrigger::VerticalWheel), 0 };
    EXPECT_EQ(ctrlShiftRule.GetModifiers(), MouseModMask::Ctrl | MouseModMask::Shift);

    MouseGestureRule::ModifiersToMods(MouseModMask::Ctrl | MouseModMask::Alt, m1, m2);
    MouseGestureRule ctrlAltRule{ m1, m2, static_cast<uint8_t>(MouseTrigger::VerticalWheel), 0 };
    EXPECT_EQ(ctrlAltRule.GetModifiers(), MouseModMask::Ctrl | MouseModMask::Alt);

    MouseGestureRule::ModifiersToMods(MouseModMask::Shift | MouseModMask::Alt, m1, m2);
    MouseGestureRule shiftAltRule{ m1, m2, static_cast<uint8_t>(MouseTrigger::VerticalWheel), 0 };
    EXPECT_EQ(shiftAltRule.GetModifiers(), MouseModMask::Shift | MouseModMask::Alt);

    AppConfig config;
    config.SetMouseGesture(
        static_cast<uint8_t>(MouseModifier::Alt), 0,
        MouseTrigger::VerticalWheel, MouseActionTarget::AdjustZoomSpeedWheel);
    config.SetMouseGesture(
        static_cast<uint8_t>(MouseModifier::Ctrl), static_cast<uint8_t>(MouseModifier::Shift),
        MouseTrigger::VerticalWheel, MouseActionTarget::Zoom);

    EXPECT_EQ(config.GetMouseGestureAction(MouseModMask::Alt, MouseTrigger::VerticalWheel),
              MouseActionTarget::AdjustZoomSpeedWheel);
    EXPECT_EQ(config.GetMouseGestureAction(MouseModMask::Ctrl | MouseModMask::Shift, MouseTrigger::VerticalWheel),
              MouseActionTarget::Zoom);
}

TEST(HotkeyRulesTest, SyncLegacyHotkeysArrayAndMiddleClick) {
    AppConfig config;
    std::array<HotkeyBinding, static_cast<size_t>(HotkeyAction::Count)> hotkeys = {};
    for (size_t i = 1; i < static_cast<size_t>(HotkeyAction::Count); ++i) {
        hotkeys[i].action = static_cast<HotkeyAction>(i);
    }

    config.SyncLegacyHotkeysArray(hotkeys);
    EXPECT_EQ(hotkeys[static_cast<size_t>(HotkeyAction::Exit)].combo.virtualKey, VK_ESCAPE);

    // Changing MiddleClick gesture from ExitApp to None must remove VK_MBUTTON from Exit rules
    config.SetMouseGesture(0, 0, MouseTrigger::MiddleClick, MouseActionTarget::None);
    config.SyncMiddleClickHotkeyFromGestures();
    bool hasMButton = false;
    for (const auto& cb : config.GetCombosForAction(HotkeyAction::Exit)) {
        if (cb.virtualKey == VK_MBUTTON && cb.modifiers == 0) hasMButton = true;
    }
    EXPECT_FALSE(hasMButton);

    // Restoring MiddleClick gesture to ExitApp must re-add VK_MBUTTON to Exit rules
    config.SetMouseGesture(0, 0, MouseTrigger::MiddleClick, MouseActionTarget::ExitApp);
    config.SyncMiddleClickHotkeyFromGestures();
    hasMButton = false;
    for (const auto& cb : config.GetCombosForAction(HotkeyAction::Exit)) {
        if (cb.virtualKey == VK_MBUTTON && cb.modifiers == 0) hasMButton = true;
    }
    EXPECT_TRUE(hasMButton);
}

TEST(HotkeySerializationTest, EnumRoundTripSerialization) {
    // Every non-None HotkeyAction must round-trip cleanly via HotkeyActionToString <-> StringToHotkeyAction
    for (size_t i = 1; i < static_cast<size_t>(HotkeyAction::Count); ++i) {
        HotkeyAction act = static_cast<HotkeyAction>(i);
        std::wstring_view serialized = HotkeyActionToString(act);
        EXPECT_FALSE(serialized.empty());
        EXPECT_NE(serialized, L"None");
        EXPECT_EQ(StringToHotkeyAction(serialized), act);
    }
}

TEST(HotkeyRulesTest, DefaultRatingRulesContainMainAndNumpadDigits) {
    AppConfig config;
    for (int d = 0; d <= 5; ++d) {
        HotkeyAction act = static_cast<HotkeyAction>(static_cast<size_t>(HotkeyAction::Rate0) + d);
        auto combos = config.GetCombosForAction(act);
        bool hasMain = false;
        bool hasNumpad = false;
        for (const auto& cb : combos) {
            if (cb.modifiers == 0 && cb.virtualKey == static_cast<uint16_t>('0' + d)) hasMain = true;
            if (cb.modifiers == 0 && cb.virtualKey == static_cast<uint16_t>(VK_NUMPAD0 + d)) hasNumpad = true;
        }
        EXPECT_TRUE(hasMain) << "Missing main keyboard rating key for " << d;
        EXPECT_TRUE(hasNumpad) << "Missing numpad rating key for " << d;
    }
}

TEST(MouseGestureTest, OrthogonalHoldModifiersAndWheelModes) {
    AppConfig config;
    config.SetMouseGesture(
        static_cast<uint8_t>(MouseModifier::HoldLeft), 0,
        MouseTrigger::VerticalWheel, MouseActionTarget::PanImage);
    config.SetMouseGesture(
        static_cast<uint8_t>(MouseModifier::HoldXButton2), static_cast<uint8_t>(MouseModifier::Ctrl),
        MouseTrigger::VerticalWheel, MouseActionTarget::Zoom);
    config.SetMouseGesture(
        0, 0, MouseTrigger::LeftRightChord, MouseActionTarget::ToggleWheelPanMode);
    config.SetMouseGesture(
        0, 0, MouseTrigger::XButton1Drag, MouseActionTarget::PanImage);
    config.SetMouseGesture(
        static_cast<uint8_t>(MouseModifier::Shift), 0,
        MouseTrigger::LeftDblClick, MouseActionTarget::ToggleWheelSwapMode);

    EXPECT_EQ(config.GetMouseGestureAction(MouseModMask::HoldLeft, MouseTrigger::VerticalWheel),
              MouseActionTarget::PanImage);
    EXPECT_EQ(config.GetMouseGestureAction(MouseModMask::HoldX2 | MouseModMask::Ctrl, MouseTrigger::VerticalWheel),
              MouseActionTarget::Zoom);
    EXPECT_EQ(config.GetMouseGestureAction(MouseModMask::None, MouseTrigger::LeftRightChord),
              MouseActionTarget::ToggleWheelPanMode);
    EXPECT_EQ(config.GetMouseGestureAction(MouseModMask::None, MouseTrigger::XButton1Drag),
              MouseActionTarget::PanImage);
    EXPECT_EQ(config.GetMouseGestureAction(MouseModMask::Shift, MouseTrigger::LeftDblClick),
              MouseActionTarget::ToggleWheelSwapMode);

    EXPECT_TRUE(config.HasAnyActiveMouseModMask(MouseModMask::HoldLeft));
    EXPECT_TRUE(config.HasAnyActiveMouseModMask(MouseModMask::HoldX2));
    EXPECT_FALSE(config.HasAnyActiveMouseModMask(MouseModMask::HoldMiddle));
}

TEST(MouseGestureTest, LegacyRightHoldWheelNormalizationAndSelfCompatibility) {
    AppConfig config;
    config.MouseGestures.clear();
    config.MouseGestures.push_back({ 0, 0, static_cast<uint8_t>(MouseTrigger::RightHoldWheel), static_cast<int>(MouseActionTarget::Navigate) });

    // Before normalization, canonical lookup must still resolve HoldRight + VerticalWheel
    EXPECT_EQ(config.GetMouseGestureAction(MouseModMask::HoldRight, MouseTrigger::VerticalWheel),
              MouseActionTarget::Navigate);

    config.NormalizeMouseGestures();
    ASSERT_EQ(config.MouseGestures.size(), 1u);
    EXPECT_EQ(config.MouseGestures[0].mod1, static_cast<uint8_t>(MouseModifier::HoldRight));
    EXPECT_EQ(config.MouseGestures[0].trigger, static_cast<uint8_t>(MouseTrigger::VerticalWheel));
    EXPECT_EQ(config.GetMouseGestureAction(MouseModMask::HoldRight, MouseTrigger::VerticalWheel),
              MouseActionTarget::Navigate);

    // Self-incompatible combinations must be rejected
    EXPECT_FALSE(IsMouseModifierTriggerCompatible(static_cast<uint8_t>(MouseModifier::HoldLeft), MouseTrigger::LeftDrag));
    EXPECT_FALSE(IsMouseModifierTriggerCompatible(static_cast<uint8_t>(MouseModifier::HoldLeft), MouseTrigger::LeftRightChord));
    EXPECT_TRUE(IsMouseModifierTriggerCompatible(static_cast<uint8_t>(MouseModifier::HoldLeft), MouseTrigger::VerticalWheel));
    EXPECT_TRUE(IsMouseModifierTriggerCompatible(static_cast<uint8_t>(MouseModifier::HoldLeft), MouseTrigger::RightClick));
    EXPECT_FALSE(IsMouseModifierTriggerCompatible(static_cast<uint8_t>(MouseModifier::HoldXButton1), MouseTrigger::XButton1Click));
    EXPECT_TRUE(IsMouseModifierTriggerCompatible(static_cast<uint8_t>(MouseModifier::HoldXButton1), MouseTrigger::VerticalWheel));
}

TEST(MouseGestureTest, TriggerActionCompatibilityMatrix) {
    // Drag triggers: support continuous spatial/hold actions; reject discrete/wheel-only actions
    for (MouseTrigger dragTrig : { MouseTrigger::LeftDrag, MouseTrigger::MiddleDrag, MouseTrigger::RightDrag,
                                    MouseTrigger::XButton1Drag, MouseTrigger::XButton2Drag }) {
        EXPECT_TRUE(IsMouseTriggerActionCompatible(dragTrig, MouseActionTarget::WindowDrag));
        EXPECT_TRUE(IsMouseTriggerActionCompatible(dragTrig, MouseActionTarget::PanImage));
        EXPECT_TRUE(IsMouseTriggerActionCompatible(dragTrig, MouseActionTarget::Zoom));
        EXPECT_TRUE(IsMouseTriggerActionCompatible(dragTrig, MouseActionTarget::CropRegionDrag));
        EXPECT_TRUE(IsMouseTriggerActionCompatible(dragTrig, MouseActionTarget::Loupe));
        EXPECT_TRUE(IsMouseTriggerActionCompatible(dragTrig, MouseActionTarget::ViewPan));
        EXPECT_FALSE(IsMouseTriggerActionCompatible(dragTrig, MouseActionTarget::PanVertical));
        EXPECT_FALSE(IsMouseTriggerActionCompatible(dragTrig, MouseActionTarget::PanHorizontal));
        EXPECT_FALSE(IsMouseTriggerActionCompatible(dragTrig, MouseActionTarget::ContextMenu));
        EXPECT_FALSE(IsMouseTriggerActionCompatible(dragTrig, MouseActionTarget::AdjustZoomSpeedWheel));
    }

    // Wheel triggers: support PanImage, PanVertical, PanHorizontal, Zoom, LockWindowZoomWheel, AdjustZoomSpeedWheel, Navigate; reject WindowDrag/Loupe/Click actions
    for (MouseTrigger wheelTrig : { MouseTrigger::VerticalWheel, MouseTrigger::HorizontalWheel }) {
        EXPECT_FALSE(IsMouseTriggerActionCompatible(wheelTrig, MouseActionTarget::WindowDrag));
        EXPECT_TRUE(IsMouseTriggerActionCompatible(wheelTrig, MouseActionTarget::PanImage));
        EXPECT_TRUE(IsMouseTriggerActionCompatible(wheelTrig, MouseActionTarget::PanVertical));
        EXPECT_TRUE(IsMouseTriggerActionCompatible(wheelTrig, MouseActionTarget::PanHorizontal));
        EXPECT_TRUE(IsMouseTriggerActionCompatible(wheelTrig, MouseActionTarget::Zoom));
        EXPECT_TRUE(IsMouseTriggerActionCompatible(wheelTrig, MouseActionTarget::LockWindowZoomWheel));
        EXPECT_TRUE(IsMouseTriggerActionCompatible(wheelTrig, MouseActionTarget::AdjustZoomSpeedWheel));
        EXPECT_TRUE(IsMouseTriggerActionCompatible(wheelTrig, MouseActionTarget::Navigate));
        EXPECT_FALSE(IsMouseTriggerActionCompatible(wheelTrig, MouseActionTarget::Loupe));
        EXPECT_FALSE(IsMouseTriggerActionCompatible(wheelTrig, MouseActionTarget::ViewPan));
        EXPECT_FALSE(IsMouseTriggerActionCompatible(wheelTrig, MouseActionTarget::ContextMenu));
        EXPECT_FALSE(IsMouseTriggerActionCompatible(wheelTrig, MouseActionTarget::CropRegionDrag));
    }

    // Click / Chord triggers: support discrete commands and wheel-mode toggles; reject Drag/Wheel-only actions and Loupe
    for (MouseTrigger clickTrig : { MouseTrigger::LeftClick, MouseTrigger::LeftDblClick, MouseTrigger::MiddleClick,
                                     MouseTrigger::RightClick, MouseTrigger::XButton1Click, MouseTrigger::XButton2Click,
                                     MouseTrigger::LeftRightChord }) {
        EXPECT_TRUE(IsMouseTriggerActionCompatible(clickTrig, MouseActionTarget::SmartZoom));
        EXPECT_TRUE(IsMouseTriggerActionCompatible(clickTrig, MouseActionTarget::ContextMenu));
        EXPECT_TRUE(IsMouseTriggerActionCompatible(clickTrig, MouseActionTarget::ToggleWheelSwapMode));
        EXPECT_TRUE(IsMouseTriggerActionCompatible(clickTrig, MouseActionTarget::ToggleWheelPanMode));
        EXPECT_FALSE(IsMouseTriggerActionCompatible(clickTrig, MouseActionTarget::WindowDrag));
        EXPECT_FALSE(IsMouseTriggerActionCompatible(clickTrig, MouseActionTarget::PanVertical));
        EXPECT_FALSE(IsMouseTriggerActionCompatible(clickTrig, MouseActionTarget::PanHorizontal));
        EXPECT_FALSE(IsMouseTriggerActionCompatible(clickTrig, MouseActionTarget::Zoom));
        EXPECT_FALSE(IsMouseTriggerActionCompatible(clickTrig, MouseActionTarget::CropRegionDrag));
        EXPECT_FALSE(IsMouseTriggerActionCompatible(clickTrig, MouseActionTarget::Loupe));
    }

    // Auto Pan supports Click (Toggle) and Drag (Hold-Drag), but rejects DoubleClick / Chord
    for (MouseTrigger singleClickTrig : { MouseTrigger::LeftClick, MouseTrigger::MiddleClick,
                                           MouseTrigger::RightClick, MouseTrigger::XButton1Click,
                                           MouseTrigger::XButton2Click }) {
        EXPECT_TRUE(IsMouseTriggerActionCompatible(singleClickTrig, MouseActionTarget::ViewPan));
    }
    EXPECT_FALSE(IsMouseTriggerActionCompatible(MouseTrigger::LeftDblClick, MouseActionTarget::ViewPan));
    EXPECT_FALSE(IsMouseTriggerActionCompatible(MouseTrigger::LeftRightChord, MouseActionTarget::ViewPan));
}

TEST(HotkeyDispatchTest, ExitShortcutsBypassEscapeLayeredDismissal) {
    AppConfig config;
    const KeyCombo pureEsc{ VK_ESCAPE, 0 };
    const KeyCombo ctrlW{ 'W', 1 };
    const KeyCombo altF4{ VK_F4, 4 };
    const KeyCombo shiftEsc{ VK_ESCAPE, 2 };

    // Default exit combos bypass layered dismissal regardless of inline focus or WM_SYSKEYDOWN
    EXPECT_EQ(ResolveKeyDispatchRoute(config.HotkeyRules, ctrlW, KeyDispatchContext{ false, false }),
              KeyDispatchRoute::DirectExitWindow);
    EXPECT_EQ(ResolveKeyDispatchRoute(config.HotkeyRules, ctrlW, KeyDispatchContext{ false, true }),
              KeyDispatchRoute::DirectExitWindow);
    EXPECT_EQ(ResolveKeyDispatchRoute(config.HotkeyRules, altF4, KeyDispatchContext{ true, false }),
              KeyDispatchRoute::DirectExitWindow);
    EXPECT_EQ(ResolveKeyDispatchRoute(config.HotkeyRules, altF4, KeyDispatchContext{ false, true }),
              KeyDispatchRoute::DirectExitWindow);

    // Pure ESC always goes to layered dismissal first; Shift+Esc uses normal hotkey dispatch
    EXPECT_EQ(ResolveKeyDispatchRoute(config.HotkeyRules, pureEsc, KeyDispatchContext{ false, false }),
              KeyDispatchRoute::PureEscapeDismiss);
    EXPECT_EQ(ResolveKeyDispatchRoute(config.HotkeyRules, pureEsc, KeyDispatchContext{ false, true }),
              KeyDispatchRoute::PureEscapeDismiss);
    EXPECT_EQ(ResolveKeyDispatchRoute(config.HotkeyRules, shiftEsc, KeyDispatchContext{ false, false }),
              KeyDispatchRoute::NormalDispatch);

    // Unbinding pure ESC from Exit keeps layered dismissal active while leaving fallback action None
    config.RemoveHotkeyRule(HotkeyAction::Exit, pureEsc);
    EXPECT_EQ(ResolveKeyDispatchRoute(config.HotkeyRules, pureEsc, KeyDispatchContext{ false, false }),
              KeyDispatchRoute::PureEscapeDismiss);
    EXPECT_EQ(FindMatchingHotkeyAction(config.HotkeyRules, pureEsc), HotkeyAction::None);

    // Single-key Exit binding ('Q') directly closes unless an inline text/numeric input is focused
    const KeyCombo keyQ{ 'Q', 0 };
    config.AddHotkeyRule(HotkeyAction::Exit, keyQ);
    EXPECT_EQ(ResolveKeyDispatchRoute(config.HotkeyRules, keyQ, KeyDispatchContext{ false, false }),
              KeyDispatchRoute::DirectExitWindow);
    EXPECT_EQ(ResolveKeyDispatchRoute(config.HotkeyRules, keyQ, KeyDispatchContext{ false, true }),
              KeyDispatchRoute::NormalDispatch);

    // Removing Alt+F4 rule preserves system Alt+F4 SC_CLOSE passthrough unless rebound to another action
    config.RemoveHotkeyRule(HotkeyAction::Exit, altF4);
    EXPECT_EQ(ResolveKeyDispatchRoute(config.HotkeyRules, altF4, KeyDispatchContext{ true, false }),
              KeyDispatchRoute::SystemAltF4Passthrough);

    config.AddHotkeyRule(HotkeyAction::ToggleFullscreen, altF4);
    EXPECT_EQ(ResolveKeyDispatchRoute(config.HotkeyRules, altF4, KeyDispatchContext{ true, false }),
              KeyDispatchRoute::NormalDispatch);
}

TEST(HotkeyCaptureTest, PureEscapeCanBeReboundToExitWhenMissing) {
    AppConfig config;
    const KeyCombo pureEsc{ VK_ESCAPE, 0 };
    const KeyCombo ctrlW{ 'W', 1 };
    const KeyCombo altF4{ VK_F4, 4 };
    const KeyCombo shiftEsc{ VK_ESCAPE, 2 };

    // When Exit already has ESC, pure ESC cancels capture for any action
    EXPECT_TRUE(ShouldCancelHotkeyCaptureOnCombo(config.HotkeyRules, HotkeyAction::Exit, pureEsc));
    EXPECT_TRUE(ShouldCancelHotkeyCaptureOnCombo(config.HotkeyRules, HotkeyAction::NavNext, pureEsc));

    // When Exit has lost its ESC rule, capturing ESC for Exit rebinds it instead of cancelling
    config.RemoveHotkeyRule(HotkeyAction::Exit, pureEsc);
    EXPECT_FALSE(ShouldCancelHotkeyCaptureOnCombo(config.HotkeyRules, HotkeyAction::Exit, pureEsc));
    EXPECT_TRUE(ShouldCancelHotkeyCaptureOnCombo(config.HotkeyRules, HotkeyAction::NavNext, pureEsc));

    // Modifier combos and other exit shortcuts never cancel capture
    EXPECT_FALSE(ShouldCancelHotkeyCaptureOnCombo(config.HotkeyRules, HotkeyAction::Exit, ctrlW));
    EXPECT_FALSE(ShouldCancelHotkeyCaptureOnCombo(config.HotkeyRules, HotkeyAction::Exit, altF4));
    EXPECT_FALSE(ShouldCancelHotkeyCaptureOnCombo(config.HotkeyRules, HotkeyAction::Exit, shiftEsc));
}






