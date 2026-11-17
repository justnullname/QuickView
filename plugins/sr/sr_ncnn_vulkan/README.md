# Real-ESRGAN NCNN Vulkan Plugin (`sr_ncnn_vulkan`)

In-process Vulkan GPU neural super-resolution plugin for **QuickView** (`com.quickview.sr.ncnn_vulkan`).

---

## Option 1: Install or Update Inside QuickView (Recommended)

For standard portable or installer builds of QuickView:

1. Open QuickView and press `,` (or right-click -> **Settings**) to open **Settings**.
2. Go to **Plugins** -> **Online Extensions**.
3. Click **Install** (or **Update** if a newer plugin version is available). QuickView will automatically download, verify, extract, and activate the plugin and its models.

---

## Option 2: Manual Installation

If you downloaded `sr_ncnn_vulkan_AMD64.zip` or `sr_ncnn_vulkan_ARM64.zip` manually, extract the **entire contents** of this archive into QuickView's plugin subfolder using the directory layout below:

```text
QuickView/
├── QuickView.exe
└── plugins/
    └── sr/
        └── sr_ncnn_vulkan/
            ├── sr_ncnn_vulkan.qvx
            ├── README.md
            └── models/
                ├── realesr-animevideov3-x2.bin
                ├── realesr-animevideov3-x2.param
                ├── realesr-animevideov3-x3.bin
                ├── realesr-animevideov3-x3.param
                ├── realesr-animevideov3-x4.bin
                ├── realesr-animevideov3-x4.param
                ├── realesr-general-x4v3.bin
                ├── realesr-general-x4v3.param
                ├── realesrgan-x4plus.bin
                ├── realesrgan-x4plus.param
                ├── realesrgan-x4plus-anime.bin
                └── realesrgan-x4plus-anime.param
```

> **Note for Program Files / Read-Only Installations**:  
> If QuickView is installed in a protected folder without write permissions (e.g. `C:\Program Files\QuickView`) and cannot write to `<QuickView>\plugins\`, place the extracted `sr_ncnn_vulkan` folder at:  
> `%LOCALAPPDATA%\QuickView\plugins\sr\sr_ncnn_vulkan\`

### Activating After Manual Extraction

1. Launch (or restart) QuickView.
2. Open **Settings -> Plugins**.
3. If the plugin does not appear immediately, click **Quick Enable Engine** or select `plugins\sr\sr_ncnn_vulkan\sr_ncnn_vulkan.qvx`.
4. Toggle **AI Super-Resolution Plugin** ON and choose your preferred neural model.

---

## Included Real-ESRGAN Models (`models/`)

Every model requires a matching `.param` + `.bin` file pair:

- `realesr-animevideov3-x2` / `-x3` / `-x4`: Ultra-fast anime & illustration upscaling (also powers **Anime Fast Auto**).
- `realesrgan-x4plus`: General-purpose photorealistic 4x upscaling.
- `realesrgan-x4plus-anime`: High-quality 4x anime & illustration upscaling.
- `realesr-general-x4v3`: Balanced lightweight general 4x upscaling.

---

## Note for Microsoft Store (MSIX) Users

The **Microsoft Store** edition already bundles `sr_ncnn_vulkan.qvx` directly inside the signed app package (`<PackageRoot>\plugins\sr\sr_ncnn_vulkan\sr_ncnn_vulkan.qvx`). You do **not** need to download or copy `.qvx` files manually.

To manage or manually place neural model weights for the Store edition, download models directly inside **Settings -> Plugins**, or click **Open Models Folder** to copy `.bin` / `.param` pairs into:

```text
%LOCALAPPDATA%\QuickView\plugins\sr\sr_ncnn_vulkan\models\
```

(Inside the Windows Store container, Windows redirects `%LOCALAPPDATA%` automatically to the app's `LocalCache\Local` storage.)

---

## Requirements & License

- **OS**: Windows 10 / 11 (64-bit `x64` or `ARM64`)
- **GPU**: Vulkan-capable graphics driver
- **License**: GNU General Public License v3.0 (GPL-3.0)
