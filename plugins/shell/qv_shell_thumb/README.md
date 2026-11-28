# Windows Explorer Thumbnail Provider Plugin (`qv_shell_thumb`)

High-speed Windows File Explorer thumbnail provider for **QuickView** (`com.quickview.shell.thumb`). Generates Explorer thumbnails for JXL, AVIF, SVG, PSD, EXR, HDR, Camera RAW, CBZ/CBR, and PDF via pooled QuickView background workers.

---

## Option 1: Install or Update Inside QuickView (Recommended)

1. Open QuickView and press `,` (or right-click -> **Settings**).
2. Go to **Plugins** -> **Marketplace**.
3. Click **Install** (or **Update**) next to **Windows Explorer Thumbnail Provider**.

---

## Option 2: Offline / Manual Installation

If you have no internet access on the target machine and downloaded `qv_shell_thumb_AMD64.zip` or `qv_shell_thumb_ARM64.zip` manually, extract the **entire contents** into QuickView's plugin subfolder:

```text
QuickView/
├── QuickView.exe
└── plugins/
    └── shell/
        └── qv_shell_thumb/
            ├── qv_shell_thumb.qvx
            ├── manifest.json
            └── README.md
```

> **Note for Installer / Program Files Installations**:  
> If QuickView is installed in a protected folder without write permissions (e.g. `C:\Program Files\QuickView`), extract `qv_shell_thumb` into your per-user plugin directory instead:  
> `%LOCALAPPDATA%\QuickView\plugins\shell\qv_shell_thumb\`

### Activating After Manual Extraction

1. Launch (or restart) QuickView.
2. Open **Settings -> Plugins -> Windows Explorer Thumbnail Provider**.
3. Toggle the plugin **ON** to register the thumbnail handler (no restart required; you can select which format groups to enable and optionally click **Refresh Shell Cache**).

---

## Requirements & License

- **OS**: Windows 10 / 11 (64-bit `x64` or `ARM64`)
- **License**: GNU General Public License v3.0 (GPL-3.0)
