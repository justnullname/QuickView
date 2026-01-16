<div align="center">

<img src="ScreenShot/main_ui.png" alt="QuickView Hero" width="100%" style="border-radius: 10px; box-shadow: 0 0 20px rgba(0,0,0,0.5);">

<br><br>

# ⚡ QuickView

### Windows 平台的高性能图像查看器。
**为速度而生。为极客打造。**

<p>
    <strong>Direct2D Native</strong> • 
    <strong>Modern C++23</strong> • 
    <strong>量子流架构 (Quantum Stream)</strong> • 
    <strong>绿色便携</strong>
</p>

<p>
    <a href="LICENSE">
        <img src="https://img.shields.io/badge/license-GPL--3.0-blue.svg?style=flat-square&logo=github" alt="License">
    </a>
    <a href="#">
        <img src="https://img.shields.io/badge/platform-Windows%2010%20%7C%2011%20(x64)-0078D6.svg?style=flat-square&logo=windows" alt="Platform">
    </a>
    <a href="https://github.com/justnullname/QuickView/releases/latest">
        <img src="https://img.shields.io/github/v/release/justnullname/QuickView?style=flat-square&label=latest&color=2ea44f&logo=rocket" alt="Latest Release">
    </a>
    <a href="#">
         <img src="https://img.shields.io/badge/arch-AVX2%20Optimized-critical?style=flat-square&logo=intel" alt="AVX2">
    </a>
</p>

<h3>
    <a href="https://github.com/justnullname/QuickView/releases/latest">📥 下载最新版</a>
    <span> • </span>
    <a href="https://github.com/justnullname/QuickView/tree/main/ScreenShot">📸 截图预览</a>
    <span> • </span>
    <a href="https://github.com/justnullname/QuickView/issues">🐛 报告 Bug</a>
</h3>

</div>

---

## 🚀 简介

**QuickView** 是目前 Windows 平台上最快的图像查看器之一。我们专注于提供极致的 **浏览体验**——把繁重的编辑工作留给 Photoshop 这样的专业工具。

使用 **Direct2D** 和 **C++23** 从头重写，QuickView 摒弃了传统的 GDI 渲染，采用了游戏级的视觉架构。它的启动速度和渲染性能足以媲美甚至超越闭源商业软件，旨在以零延迟处理从微小图标到巨型 8K RAW 照片的所有内容。

### 📂 支持格式
QuickView 支持几乎所有现代和专业图像格式：

* **经典格式：** `JPG`, `JPEG`, `PNG`, `BMP`, `GIF`, `TIF`, `TIFF`, `ICO`
* **现代/Web格式：** `WEBP`, `AVIF`, `HEIC`, `HEIF`, `SVG`, `SVGZ`, `JXL`
* **专业/HDR：** `EXR`, `HDR`, `PIC`, `PSD`, `TGA`, `PCX`, `QOI`, `WBMP`, `PAM`, `PBM`, `PGM`, `PPM`, `WDP`, `HDP`
* **RAW (LibRaw)：** `ARW`, `CR2`, `CR3`, `DNG`, `NEF`, `ORF`, `RAF`, `RW2`, `SRW`, `X3F`, `MRW`, `MOS`, `KDC`, `DCR`, `SR2`, `PEF`, `ERF`, `3FR`, `MEF`, `NRW`, `RAW`

---

# QuickView v3.0.4 - 量子流更新 (The Quantum Flow Update)
**发布日期**: 2026-01-16

### ⚡ 核心架构："Quantum Flow"
- **统一调度与解码 (Quantum Flow)**：引入了“快/慢双通道”架构 (`FastLane` + `HeavyLanePool`)，将即时交互与繁重的解码任务隔离。
- **N+1 热备架构**：实现了“上限 N+1”线程模型，备用线程保持预热状态以立即响应，在不过度订阅的情况下最大化 CPU 吞吐量。
- **深度取消 (Deep Cancellation)**：为重型格式 (JXL/RAW/WebP) 提供了细粒度的“按需”取消逻辑，确保过时的任务（例如在快速滚动期间）被立即终止以节省功耗。
- **Direct D2D 直通**：建立了“零拷贝”管道，解码后的 `RawImageFrame` 缓冲区直接上传到 GPU 显存，完全绕过 GDI/GDI+。

### 🎨 视觉与渲染重构
- **DirectComposition (游戏级渲染)**：彻底放弃了传统的 SwapChain/GDI 模型，转而使用 `DirectComposition` 视觉树。
    - **Visual Ping-Pong**：实现了双缓冲 Visual 架构，实现无撕裂、无伪影的交叉淡入淡出。
    - **IDCompositionScaleTransform**：硬件加速的高精度缩放和平移。
- **原生 SVG 引擎**：用 **Direct2D Native SVG** 渲染取代了 `nanosvg`。
    - **能力**：支持复杂的 SVG 滤镜、渐变和 CSS 透明度。
    - **2级无损缩放**：深度缩放期间进行基于矢量的重新光栅化，实现无限清晰度。
    - *(要求：Windows 10 Creators Update 1703 或更高版本)*。

### 💾 内存与资源管理
- **Arena 动态分配 (智能内存)**：切换到使用多态内存资源 (PMR) 的 **TripleArena** 策略。内存被预先分配并循环使用 (Bucket Strategy)，彻底消除堆碎片。
- **智能定向预读 (Smart Prefetch)**：
    - **自动调优**：根据检测到的系统 RAM 自动选择 `Eco` (节能), `Balanced` (平衡), 或 `Performance` (性能) 预读策略。
    - **手动覆盖**：用户可完全控制缓存行为。
    - **智能跳过 (Smart Skip)**：在节能模式下，通过智能跳过超过缓存预算的任务来防止 "OOM" (内存溢出) 。

### 🧩 基础设施与元数据
- **元数据架构重构**：将“快速文件头窥视 (Fast Header Peeking)”（用于即时布局）与“异步富元数据 (Async Rich Metadata)”解析 (Exif/IPTC/XMP) 解耦，解决了 UI 阻塞问题。
- **调试 HUD**：添加了实时“矩阵”叠加层 (`F12`)，可视化缓存拓扑、工作通道状态和帧时序。

---

## ✨ 核心功能

### 1. 🏎️ 极致性能
> *"速度即功能。"*

QuickView 利用 **多线程解码** 技术处理 **JXL** 和 **AVIF** 等现代格式，在 8 核 CPU 上相比标准查看器加载速度提升高达 **6倍**。
* **零延迟预览：** 针对巨型 RAW (ARW, CR2) 和 PSD 文件的智能提取技术。
* **调试 HUD：** 按 `F12` 查看实时性能指标（解码时间、渲染时间、内存使用）。

### 2. 🎛️ 可视化控制中心
> *告别手动编辑 .ini 文件。*

<img src="ScreenShot/settings_ui.png" alt="Settings UI" width="100%" style="border-radius: 6px;">

完全硬件加速的 **设置仪表板**。
* **精细控制：** 调整鼠标行为（平移 vs 拖拽）、缩放灵敏度和循环规则。
* **视觉个性化：** 实时调整 UI 透明度和背景网格。
* **便携模式：** 一键切换配置存储位置（AppData/系统 还是 程序文件夹/USB）。

### 3. 📊 极客可视化
> *不只是看图；更要洞察数据。*

<div align="center">
  <img src="ScreenShot/geek_info.png" alt="Geek Info" width="48%">
  <img src="ScreenShot/photo_wall.png" alt="Photo Wall" width="48%">
</div>

* **实时 RGB 直方图：** 半透明波形叠加。
* **重构的元数据架构：** 更快、更准确的 EXIF/元数据解析。
* **HUD 照片墙：** 按 `T` 召唤高性能画廊叠加层，能够以 60fps 虚拟化滚动 10,000+ 张图片。
* **智能后缀修正：** 自动检测并修复错误的扩展名（如将 PNG 误存为 JPG）。
* **一键 RAW 渲染：** 极速切换 RAW 文件的“内嵌预览”与“完整解码”模式。
* **专业色彩分析：** 实时显示 **色彩空间** (sRGB/P3/Rec.2020)、**色彩模式** (YCC/RGB/CMYK) 和 **压缩质量** (Q-Factor)。

---

## ⚙️ 引擎室

我们不使用通用编解码器。我们为每种格式选用 **最先进 (State-of-the-Art)** 的库。

| 格式 | 后端引擎 | 为什么它很棒 (架构) |
| :--- | :--- | :--- |
| **JPEG** | **libjpeg-turbo v3** | **AVX2 SIMD**。解压速度之王。 |
| **PNG / QOI** | **Google Wuffs** | **内存安全**。超越 libpng，轻松处理超大尺寸。 |
| **JXL** | **libjxl + threads** | **并行化**。高分辨率 JPEG XL 即时解码。 |
| **AVIF** | **dav1d + threads** | **汇编优化** 的 AV1 解码。 |
| **SVG** | **Direct2D Native** | **硬件加速**。无限无损缩放。 |
| **RAW** | **LibRaw** | 针对“即时预览”提取进行了优化。 |
| **EXR** | **TinyEXR** | 轻量级、工业级 OpenEXR 支持。 |
| **HEIC / TIFF**| **Windows WIC** | 硬件加速（需要系统扩展）。 |

---

## ⌨️ 快捷键

掌握这些即可随心所欲地浏览：

| 分类 | 按键 | 动作 |
| :--- | :--- | :--- |
| **导航** | `Space` / `PgDn` | 下一张图片 |
| | `Bksp` / `PgUp` | 上一张图片 |
| | `T` | **照片墙 (HUD)** |
| **视图** | `1` / `Z` | **100% 原始尺寸** |
| | `0` / `F` | 适应屏幕 |
| | `Enter` | 全屏 |
| **信息** | `I` | **切换 信息/直方图** |
| | `D` | **切换 调试 HUD** |
| **控制** | `Ctrl + P` | **设置面板** |
| | `Ctrl + T` | 切换 "总在最前" |
| **编辑** | `R` | 旋转 |
| | `Del` | 删除文件 |

---

🗺️ 开发计划 (Roadmap)
我们因为持续进化而卓越。以下是当前正在开发的功能：

- **动画支持 (Animation Support):** GIF/WebP/APNG 完整播放支持。
- **帧查看器 (Frame Inspector):** 暂停并逐帧分析动画。
- **色彩管理 (CMS):** 完整的 ICC 配置文件支持。
- **双图比对 (Dual-View Compare):** 并排比较两张图片。
- **智能背景 (Smart Background):** 自动变暗 / 亚克力模糊效果。

---

## 📥 安装

**QuickView 是 100% 绿色便携的。**

1.  前往 [**Releases**](https://github.com/justnullname/QuickView/releases).
2.  下载 `QuickView.zip`.
3.  解压到任意位置并运行 `QuickView.exe`.
4.  *(可选)* 使用应用内设置将其注册为默认查看器。

---

## ⚖️ 致谢

**QuickView** 站在巨人的肩膀上。
基于 **GPL-3.0** 许可。
特别感谢 **David Kleiner** (JPEGView 原作者) 以及 **LibRaw, Google Wuffs, dav1d, 和 libjxl** 的维护者们。
