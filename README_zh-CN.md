<div align="center">

<img src="ScreenShot/main_ui.png" alt="QuickView Hero" width="100%" style="border-radius: 10px; box-shadow: 0 0 20px rgba(0,0,0,0.5);">

<br><br>

# ⚡ QuickView

### Windows 平台的高性能图像查看器。
**为速度而生。为极客打造。**

<p>
    <strong>Direct2D Native</strong> • 
    <strong>Modern C++23</strong> • 
    <strong>动态调度架构 (Dynamic Scheduling)</strong> • 
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
         <img src="https://img.shields.io/badge/arch-ARM64%20%7C%20x64-darkred?style=flat-square&logo=cpu" alt="Architecture Support">
    </a>
    <a href="#">
         <img src="https://img.shields.io/badge/simd-Highway%20SSE4%2B-blue?style=flat-square&logo=google" alt="Highway SIMD">
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

🌐 **多语言支持:** English, 简体中文, 繁體中文, 日本語, Deutsch, Español, Русский

### 📂 支持格式
QuickView 支持几乎所有现代和专业图像格式：

* **经典格式：** `JPG`, `JPEG`, `PNG`, `BMP`, `GIF`, `TIF`, `TIFF`, `ICO`
* **现代/Web格式：** `WEBP`, `AVIF`, `HEIC`, `HEIF`, `SVG`, `SVGZ`, `JXL`
* **专业/HDR：** `EXR`, `HDR`, `PIC`, `PSD`, `TGA`, `PCX`, `QOI`, `WBMP`, `PAM`, `PBM`, `PGM`, `PPM`, `WDP`, `HDP`
* **RAW (LibRaw)：** `ARW`, `CR2`, `CR3`, `DNG`, `NEF`, `ORF`, `RAF`, `RW2`, `SRW`, `X3F`, `MRW`, `MOS`, `KDC`, `DCR`, `SR2`, `PEF`, `ERF`, `3FR`, `MEF`, `NRW`, `RAW`

---

# QuickView v5.0.0 - 高级色彩与架构升级
**发布日期**: 2026-04-05

### 🚀 Google Highway & ARM64 支持
- **全链路加速**: 将核心 SIMD 算子迁移至 **Google Highway**, 确保在从老旧 **SSE4** 到现代 **AVX-512** 的所有 CPU 上均能实现极致性能。
- **原生 ARM64**: 完美支持 Windows on ARM 架构，通过 NEON 指令集实现原生硬件级加速。

### 🌈 越级 HDR：全新渲染管线
- **scRGB 线性流**: 整个渲染链路升级至 32 位浮点线性空间，彻底消除色阶断层，还原最真实的色彩细节。
- **Ultra HDR GPU 合成**: 硬件加速支持 **Gain Maps** (Google/Samsung/Apple 增益图), 在 HDR 显示器上呈现令人惊叹的真实亮度。
- **色调映射 v2**: 引入专业级 HDR-SDR 滚降映射算法，在普通显示器上也能获得均衡的视觉效果。

### 🎨 色彩管理与专业软打样
- **硬件 CMS**: 将 ICC 配置文件管理深度集成至 GPU 渲染侧，支持双节点色彩空间变换。
- **全局软打样 (Soft Proofing)**: 具备专业级输出仿真功能，一键预览 CMYK、打印机或印刷配置文件效果。
- **V4 & 紧凑型配置**: 完美兼容最新的 ICC v4 及紧凑型色彩配置文件。

### 🧭 导航与排序进阶 (#118)
- **自然排序**: 浏览顺序现在与 Windows 资源管理器的自然数字逻辑完全一致。
- **自定义循环规则**: 独立拆分文件夹循环与子目录遍历开关，提供更精细的浏览控制。

---

## ✨ 核心功能

### 1. 🏎️ 极致性能
> *"速度即功能。"*

QuickView 利用 **多线程解码** 技术处理 **JXL** 和 **AVIF** 等现代格式，在 8 核 CPU 上相比标准查看器加载速度提升高达 **6倍**。
* **零延迟预览：** 针对巨型 RAW (ARW, CR2) 和 PSD 文件的智能提取技术。
* **调试 HUD：** 按 `F12` 查看实时性能指标（解码时间、渲染时间、内存使用）。*(首次使用请在 **设置 > 高级** 中开启调试模式)*
  <br><img src="ScreenShot/DebugHUD.png" alt="调试模式" width="100%" style="border-radius: 6px; margin-top: 10px;">

### 2. 🎛️ 可视化控制中心
> *告别手动编辑 .ini 文件。*

<img src="ScreenShot/settings_ui.png" alt="Settings UI" width="100%" style="border-radius: 6px;">

完全硬件加速的 **设置仪表板**。
* **精细控制：** 调整鼠标行为（平移 vs 拖拽）、缩放灵敏度和循环规则。
* **视觉个性化：** 实时调整 UI 透明度和背景网格。
* **便携模式：** 一键切换配置存储位置（AppData/系统 还是 程序文件夹/USB）。

### 3. 🔄 智能更新
> *让软件时刻保持最新。*

QuickView 会自动检测新版本，并支持一键静默更新。无需打开浏览器，即刻体验最新功能。

### 4. 📊 极客可视化
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

### 5. 🔍 极致视觉对比
> *"以前所未有的精度并行比对。"*

QuickView 提供了专为深度视觉分析打造的 **双图比对模式 (Compare Mode)**。
* **双路同步：** 两个窗格之间的缩放、平移和旋转完全同步，支持毫米级的精细核对。
* **极客 HUD：** 实时显示 **RGB 包络直方图** 和图像质量指标（熵、锐度），帮助您快速识别更优质的样张。
* **智能分割线：** 带有智能透明度的分割线，自动标注每个对比维度的“优胜者”。
  <br>![基础比对演示](ScreenShot/compare_mode.gif)
  <br>![HUD分析演示](ScreenShot/compare_mode2.gif)

### 6. 🌈 HDR 与色彩之巅
> *"像光本身一样观察。"*

QuickView 5.0 引入了工业级色彩工具链。
*   **真实 HDR 面板**：基于 SIMD 加速的实时峰值亮度估算与 "HDR Pro" 元数据（MaxCLL/FALL）分析。
*   **全局软打样**：瞬时模拟印刷效果或特殊色彩空间，支持自定义 ICC 映射。
*   **全线性工作流**：内部 32 位浮点管线，确保无带宽阶梯感与绝对色准。

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

## ⌨️ 快捷键与帮助

> *随时按 `F1` 呼出交互式快捷键指南。*

<div align="center">
  <img src="ScreenShot/help_ui.png" alt="帮助窗口" width="100%" style="border-radius: 10px; box-shadow: 0 0 20px rgba(0,0,0,0.5);">
</div>

🗺️ 开发计划 (Roadmap)
我们因为持续进化而卓越。以下是当前正在开发的功能：

- **动画支持 (Animation Support):** GIF/WebP/APNG 完整播放支持。
- **帧查看器 (Frame Inspector):** 暂停并逐帧分析动画。
- **色彩管理 (CMS):** 完整的 ICC 配置文件支持。
- **临摹模式 (Tracing Mode):** 半透明的薄膜覆盖模式，适用于设计师参考图及临摹描绘。

---

## 💻 系统要求

| 组件 | 最低要求 | 备注 |
| :--- | :--- | :--- |
| **操作系统** | Windows 10 (1511+) | 需要 DirectComposition Visual3 支持 |
| **CPU** | 支持 SSE4 指令集的处理器 | **大幅扩展硬件覆盖** (Intel 2008+ / AMD 2011+) |
| **架构** | x64 或 ARM64 | Windows on ARM 原生 NEON 优化 |

> ⚠️ **重要提示:** QuickView 现已集成 **Google Highway**，通过动态 SIMD 调度显著提升了老旧硬件的适应性。虽然要求降至 **SSE4**，但现代 CPU 用户仍可享受全速 NEON 或 AVX 带来的性能收益。

---

## 📥 安装

**QuickView 是 100% 绿色便携的。**

1.  前往 [**Releases**](https://github.com/justnullname/QuickView/releases).
2.  下载 `QuickView.zip`.
3.  解压到任意位置并运行 `QuickView.exe`.
4.  *(可选)* 使用应用内设置将其注册为默认查看器。

---

## ⚖️ 致谢鸣谢

> [!NOTE]
> **开发者寄语**
>
> 我利用业余时间维护 QuickView，只因我相信 Windows 值得拥有一个更快、更纯粹的看图工具。
> 我没有推广预算，也没有团队。如果 QuickView 对您有所帮助，在 GitHub 上点一颗星或分享给朋友，就是对我最大的支持。

**QuickView** 站在巨人的肩膀上。
基于 **GPL-3.0** 许可。
特别感谢 **David Kleiner** (JPEGView 原作者) 以及 **LibRaw, Google Wuffs, dav1d, 和 libjxl** 的维护者们。
特别感谢 **@Dimmitrius** 对俄罗斯语翻译进行的深度优化。
