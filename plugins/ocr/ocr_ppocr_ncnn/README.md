# PP-OCRv5 NCNN Vulkan Analyzer Plugin (`com.quickview.ocr.ppocr_ncnn`)

High-accuracy offline OCR engine for QuickView powered by **NCNN + Vulkan GPU** (with automatic multi-threaded CPU fallback) and zero OpenCV runtime bloat.

## Built-in Worldwide Presets (One-Click Download)

Open **Settings -> Plugins -> PP-OCRv5 NCNN Vulkan** to switch or download curated FP16 model packs:

| Preset ID | Languages | Approx. Size |
|---|---|---|
| `ppocrv5_cjk_en` | Simplified/Traditional Chinese + English + Japanese | ~8.1 MB |
| `ppocrv5_latin` | Latin scripts (EN, DE, ES, FR, PT, IT) | ~8.5 MB |
| `ppocrv5_eslav` | Cyrillic scripts (RU, UK, BE + EN) | ~8.5 MB |
| `ppocrv5_korean` | Korean + English | ~9.2 MB |

## Dropping Custom Compatible Models (No Hardcoding)

Click **Open Model Folder** on the plugin card and place NCNN models converted via `pnnx` in either layout:

1. **Flat triplet** directly in `models/`:
   - `<name>.param`, `<name>.bin`, `<name>.dict.txt` (uses `ppocrv5_mobile_det` by default)
   - Optional `<name>.json` for custom detector/display metadata
2. **Subdirectory pack** in `models/<pack_name>/`:
   - `rec.param`, `rec.bin`, `dict.txt` (or single `*.param`/`*.bin`/`*.txt`)
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
