# PP-OCRv5 NCNN Vulkan Analyzer Plugin (`com.quickview.ocr.ppocr_ncnn`)

High-accuracy offline OCR engine for QuickView powered by **NCNN + Vulkan GPU** (with automatic multi-threaded CPU fallback) and zero OpenCV runtime bloat.

---

## Option 1: Install or Update Inside QuickView (Recommended)

1. Open QuickView and press `,` (or right-click -> **Settings**).
2. Go to **Plugins** -> **Marketplace** and install **PP-OCRv5 NCNN Vulkan**.
3. Expand the **PP-OCRv5 NCNN Vulkan** card, pick your preferred language pack preset, and click **Download Model** (~8–10 MB).

---

## Option 2: Offline / Manual Installation

If you are in an offline environment and downloaded `ocr_ppocr_ncnn_AMD64.zip` or `ocr_ppocr_ncnn_ARM64.zip` manually, extract the archive into QuickView's plugin subfolder:

```text
QuickView/
├── QuickView.exe
└── plugins/
    └── ocr/
        └── ocr_ppocr_ncnn/
            ├── ocr_ppocr_ncnn.qvx
            ├── manifest.json
            ├── README.md
            └── models/               <-- Place offline model files here
```

> **Note for Installer / Program Files Installations**:  
> If QuickView is installed in a protected folder without write permissions (e.g. `C:\Program Files\QuickView`), extract `ocr_ppocr_ncnn` to:  
> `%LOCALAPPDATA%\QuickView\plugins\ocr\ocr_ppocr_ncnn\`  
> And place offline model files under `%LOCALAPPDATA%\QuickView\plugins_data\com.quickview.ocr.ppocr_ncnn\models\` (or click **Open Model Folder** on the plugin card).

---

## Built-in Worldwide Presets

| Preset ID | Languages | Model Files in `models/` | Approx. Size |
|---|---|---|---|
| `ppocrv5_cjk_en` | Simplified/Traditional Chinese + English + Japanese | `ppocrv5_mobile_det.{param,bin}`, `ppocrv5_mobile_cjk_rec.{param,bin}`, `ppocrv5_cjk.dict.txt` | ~8.1 MB |
| `ppocrv5_latin` | Latin scripts (EN, DE, ES, FR, PT, IT) | `ppocrv5_mobile_det.{param,bin}`, `ppocrv5_mobile_latin_rec.{param,bin}`, `ppocrv5_latin.dict.txt` | ~8.5 MB |
| `ppocrv5_eslav` | Cyrillic scripts (RU, UK, BE + EN) | `ppocrv5_mobile_det.{param,bin}`, `ppocrv5_mobile_eslav_rec.{param,bin}`, `ppocrv5_eslav.dict.txt` | ~8.5 MB |
| `ppocrv5_korean` | Korean + English | `ppocrv5_mobile_det.{param,bin}`, `ppocrv5_mobile_korean_rec.{param,bin}`, `ppocrv5_korean.dict.txt` | ~9.2 MB |

---

## Dropping Custom Compatible Models (No Hardcoding)

Click **Open Model Folder** on the plugin card and place NCNN models converted via `pnnx` in either layout:

1. **Flat triplet** directly in `models/`:
   - `<name>.param`, `<name>.bin`, `<name>.dict.txt` (uses `ppocrv5_mobile_det` by default)
   - Optional `<name>.json` for custom detector/display metadata
2. **Subdirectory pack** in `models/<pack_name>/`:
   - `rec.param`, `rec.bin`, `dict.txt`
   - Optional `det.param` + `det.bin` (falls back to `models/ppocrv5_mobile_det.*`)
   - Optional `model.json`:
     ```json
     {
       "display_name": "My Custom Language",
       "det_model": "det",
       "rec_model": "rec",
       "dict": "dict.txt",
       "rec_height": 48
     }
     ```
