# QuickView 2.0 Settings & HUD Design

## 1. Overview
**Philosophy**: Immersive HUD (Heads-Up Display). Not a dialog box, but a semi-transparent overlay.
**Trigger**: Right-click context menu -> Settings, or `Ctrl+,`.
**Appearance**: Frameless, rounded rectangle, dark glass background (Alpha 0.9 + Blur). No "Apply" buttons - changes are instant.

---

## 2. Menu Structure (Tabs)

### 🔵 Tab 1: General (常规)
*Foundation & Lifecycle*
| Group | Item | Type | Default | Note |
| :--- | :--- | :--- | :--- | :--- |
| **Language** | Interface Language | Dropdown | Auto | Auto / EN / CN |
| **Startup** | Single Instance | Toggle | **On** | Prevent multiple windows |
| | Remember Window Pos | Toggle | Off | Restore coordinates |
| | Check Updates | Toggle | **On** | Silent logic |
| **Habits** | Loop Navigation | Toggle | **On** | Last -> First |
| | Confirm Delete | Toggle | **On** | Safety |
| | Portable Mode | Toggle | - | Config next to binary |

### 🔵 Tab 2: View (界面)
*Visuals & Window Behavior*
| Group | Item | Type | Default | Note |
| :--- | :--- | :--- | :--- | :--- |
| **Background** | Canvas Color | Radio | Black | Black / White / Grid / Custom (Picker) |
| **Window** | Always on Top | Toggle | Off | |
| | Resize on Zoom | Toggle | **On** | Window hugs image |
| | Auto-Hide Controls | Toggle | **On** | Window controls fade out |
| **Panel** | Lock Bottom Toolbar | Toggle | **On** | |
| | EXIF Panel Mode | Dropdown | Lite | Off / Lite / Full |
| | Custom Lite Tags | Input | ISO, F..| Defines Lite View content |

### 🟣 Tab 3: Control (操作)
*Input & Gestures*
| Group | Item | Type | Default | Note |
| :--- | :--- | :--- | :--- | :--- |
| **Mouse** | Invert Wheel | Toggle | Off | Zoom/Nav direction |
| | Left Drag | Dropdown | Move Win | Move Window / Pan Image / None |
| | Middle Drag | Dropdown | Pan Img | Move Window / Pan Image / None |
| | Middle Click | Dropdown | Exit | Exit / Fit / None |
| **Edge** | Edge Nav Click | Checkbox | On | Click screen edge to prev/next |
| | Nav Indicator | Dropdown | Arrow | Visual feedback style |

### 🟠 Tab 4: Image & Edit (图像与编辑)
*Rendering & IO*
| Group | Item | Type | Default | Note |
| :--- | :--- | :--- | :--- | :--- |
| **Render** | Auto Rotate (EXIF) | Toggle | **On** | Correct orientation |
| | CMS (Color Mgmt) | Toggle | Off | ICC Profiles (Perf cost) |
| **Save** | Silent Save | Checkbox | - | Overwrite without prompting |
| | Lossless Transform | Checkbox | Off | JPEG Rotation |
| | Lossy Transform | Checkbox | Off | Re-encoding allowed |
| | Edge Clip | Checkbox | Off | For partial blocks |
| **System** | File Associations | Button | - | Windows Default Apps |

### ⚫ Tab 5: About (关于)
*Meta & Credits*
* **Header**: Large Logo, Version (Build Date + Hash).
* **Credits**: Badges for D2D, TurboJPEG, Wuffs, mimalloc, etc.
* **Developer**: Name, Website, GitHub.
* **Debug**:
    * Config Path (Link).
    * [ ] Show Decode Time on HUD.

---

## 3. Technical Implementation (D2D Widget Kit)

### Architecture: Data-Driven
Avoid hardcoded coordinates. Use a structure binding UI labels to direct pointers of the `Config` struct.

```cpp
enum class OptionType { Toggle, Slider, Segment, ActionButton, ColorPicker, Input };

struct SettingsItem {
    std::wstring label;
    OptionType type;
    
    // Binding Pointers (Direct access to runtime config)
    bool* pBoolVal = nullptr; 
    float* pFloatVal = nullptr; 
    int* pIntVal = nullptr;
    std::wstring* pStrVal = nullptr;

    // Constraints
    float minVal = 0, maxVal = 100;
    std::vector<std::wstring> options; // For Segment/Dropdown
    
    // Runtime Layout
    D2D1_RECT_F layoutRect; 
};
```

### Widget Library (Direct2D)

#### A. The Toggle (开关)
*   **Visual**: Rounded slot + Circular knob.
*   **Animation**: `CurrentX = Lerp(CurrentX, TargetX, 0.2f)` for springy feel.
*   **Use Case**: Booleans (Auto Update, Always on Top).

#### B. The Slider (滑块)
*   **Visual**: Thin track (grey), active track (accent color), thumb (circle).
*   **Meta**: Draw value text next to it (e.g., "500 MB").
*   **Use Case**: Values (Opacity, Sensitive, Cache Size).

#### C. The Segment Control (分段)
*   **Visual**: Connected rectangles. Active segment is filled.
*   **Use Case**: Enums (Theme: Auto/Dark/Light).

#### D. Color Dots
*   **Visual**: 5-6 preset circles (Brand colors + Mono).
*   **Use Case**: Background color selection.

### Integration Plan
1.  **Events**: Hook `WM_LBUTTONDOWN`, `WM_MOUSEMOVE` in the Settings Overlay window/layer.
2.  **Hit Test**: Iterate `m_items`, check collision with `layoutRect`.
3.  **Update**: On click/drag, update the `*pVal` directly and trigger `InvalidateRect`.
4.  **Config**: Save `g_Config` to JSON on close or immediately on change (debounce).
