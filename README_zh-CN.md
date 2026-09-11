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

# QuickView v6.8.0 - 悬浮控制胶囊、胶片画廊与自定义快捷键
**发布日期**: 2026-06-14

*💡 小贴士：可随时按 **F1** 键呼出交互式快捷键与帮助面板，查看最新的操作方法与说明。*

### 🏝️ “灵动岛”悬浮控制胶囊
- **悬浮胶囊设计**：重构窗口控制按钮，悬浮于右上角并支持磨砂发光悬浮动效。
- **紧凑布局**：缩小控制按钮 of 占用面积，为图像显示留出更多屏幕空间。

### 🎞️ 顶部悬停胶片画廊
- **多模式唤出**：支持顶部悬停自动展开、固定展示或关闭，可通过设置自由调节。
- **平滑居中动画**：切换图片时，下方胶片缩略图支持平滑的自动居中滚动动画，提升浏览连贯性。

### 📺 Picasa 风格聚光灯幻灯片
- **双模式幻灯片**：支持传统全屏幻灯片与全新的聚光灯投影模式（背景暗淡，仅高亮展示图片及柔和光晕）。

### ⌨️ 期待已久的自定义快捷键
- **完全自定义按键**：支持在设置面板中完全自定义键盘快捷键，可根据个人习惯随心绑定核心操作与切图快捷键。

### ⚡ 极致二进制体积压缩
- **移除 C++ Stream**：剔除 `<iostream>` 依赖以缩减约 18.5 KB 二进制体积。
- **本地化模板优化**：重构本地化字符表，消除模板冗余，节省约 10.5 KB。
- **矢量图标压缩**：将图标坐标压缩为 16 位整型，减小 54 KB 体积。
- **核心去虚化**：采用 C 风格函数指针代替 `std::function`，减少虚函数开销。

---

## ✨ 核心功能

> *"速度即功能。"*

QuickView 利用 **多线程解码** 技术处理 **JXL** 和 **AVIF** 等现代格式，在 8 核 CPU 上相比标准查看器加载速度提升高达 **6倍**。
* **零延迟预览：** 针对巨型 RAW (ARW, CR2) 和 PSD 文件的智能提取技术。
* **调试 HUD：** 按 `F12` 查看实时性能指标（解码时间、渲染时间、内存使用）。*(首次使用请在 **设置 > 高级** 中开启调试模式)*
  <br><img src="ScreenShot/DebugHUD.png" alt="调试模式" width="100%" style="border-radius: 6px; margin-top: 10px;">

### 2. 🎛️ 可视化控制与主题定制
> *自定义你专属的极客看图面板。*

<img src="ScreenShot/settings_ui.png" alt="Settings UI" width="100%" style="border-radius: 6px;">

完全硬件加速的 **设置仪表板**。
* **精细控制：** 调整鼠标行为（平移 vs 拖拽）、缩放灵敏度和循环规则。
* **期待已久的自定义快捷键：** 完全支持键盘快捷键重映射，直接在设置中绑定和定制您的工作流热键。
* **定制主题与强调色：** 完美支持深色、浅色及随系统自动同步的 UI，可精细化控制界面强调色与文本颜色。
* **环境遮罩 (Ambient Dimmer)：** 启用沉浸式遮罩，在呼出菜单或画廊时自动暗化背景，让视觉重心专注在当前图像。
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

### 7. 📚 漫画模式与零开销压缩包直接查看
> *无盘写的极速虚拟读取。*

集成定制轻量级 `unrar-mini` 引擎与零开销数据导向设计 (DOD) 虚拟文件系统 (VFS)。
*   **内存零盘写读取：** 无需解压直接以内存流读取 `.zip`、`.rar`、`.cbz`、`.cbr` 漫画压缩文件，零垃圾产生，保护 SSD 寿命。
*   **漫画双页拼接：** 自动将相邻页面左右拼合显示，搭配智能动态缩放，完美还原跨页画作的连贯气势。

### 8. 🎬 动画全格式支持与脏矩形调试
> *不止于播放，更利于开发与设计。*

*   **主流动画全兼容：** 高性能、多线程解码播放 `.gif`、`.webp`、`.apng` 以及 `.avifs` 动画格式。
*   **帧查看器 (Frame Inspector)：** 随时按下 `Alt + 左右方向键` 进行逐帧步进与分析。
*   **脏矩形 (Dirty Rect) 可视化：** 专为设计师和开发者引入，实时标注 GPU 当前正在解码和刷新渲染的精确区域。

### 9. 📐 临摹模式与鼠标点击穿透
> *为设计师和插画师量身定制。*

*   **半透明悬浮视口 (Tracing Mode)：** 利用 DirectComposition 渲染出高度自定义的半透明浮动窗口，可通过快捷键调整图层透明度。
*   **鼠标点击穿透：** 开启后，鼠标点击将直接透传至下方的画板或设计软件（`WM_EX_TRANSPARENT`），是临摹、描红的绝佳利器。

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

🗺️ 开发计划🗺️ Roadmap
我们因为持续进化而卓越。以下是当前正在开发的功能：

- **临摹模式 (Tracing Mode)**: 半透明的薄膜覆盖模式，适用于设计师参考图及临摹描绘。
- **视频墙分屏 (Video Wall Port)**: 支持多显示器视觉阵列输出。
- **插件系统 (Plugin System)**: 开放式架构，支持社区开发编解码器与滤镜插件。

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

QuickView 的核心设计理念是 **绿色与便携**。

### 🍃 推荐方案：绿色便携模式
1. 从 [**Releases**](https://github.com/justnullname/QuickView/releases) 下载 `QuickView_*.zip` 压缩包。
2. 解压到任意位置并运行 `QuickView.exe`。
3. 在应用内 **设置 > 便携模式** 中管理配置文件存储位置。

### 📦 安装版与 WinGet
如果您需要标准的 Windows 安装程序（内置深度卸载工具）：
*   **手动安装**：从 [**Releases**](https://github.com/justnullname/QuickView/releases) 下载 `QuickView_Installer_*.exe`。
*   **命令行安装**：
    ```powershell
    winget install justnullname.QuickView
    ```

---

## 🛠️ 从源码编译

> ⚠️ **架构说明**：本项目已全面迁移至 `CMake + Ninja + vcpkg` 构建系统，采用 **Clang-cl** 编译器和 **Full LTO** 链接时优化以实现极限体积压缩。旧版的 `.sln` 和 `.vcxproj` 已被弃用。

### 前置要求
1. **Visual Studio 2022** (安装 "使用 C++ 的桌面开发" 工作负载) —— 提供 Windows SDK 以及 Clang-cl 工具链所需的 MSVC 库。
2. **LLVM / Clang 工具链** (确保 `clang-cl.exe` 和 `lld-link.exe` 已添加至系统环境变量 `PATH` 中；VS 自带的 LLVM (位于 `<VS>\VC\Tools\Llvm\x64\bin`) 也会被自动识别。若 LLVM 安装在其他位置，可设置环境变量 `LLVM_INSTALL_DIR` 指向 LLVM 根目录，例如 `D:\dev\lang\clang`)。
3. **CMake** (3.25+) 和 **Ninja** (可由 VS 自带或单独安装)。
4. **Git**。

### 1. 克隆仓库（含子模块）
`vcpkg`、`wuffs`、`unrar-mini` 等依赖以 Git 子模块形式提供，构建时必须一并拉取：

```powershell
git clone --recursive https://github.com/justnullname/QuickView.git
```

若克隆时未加 `--recursive`，可事后补拉：

```powershell
git submodule update --init --recursive
```

### 2. 初始化 vcpkg（仅需一次）
```powershell
.\third_party\vcpkg\bootstrap-vcpkg.bat
```

### 3. 配置与编译
在根目录下依次执行以下两条咒语：

```powershell
# 1. 调用 vcpkg 拉取依赖并配置 Release-LTO 构建矩阵
cmake --preset Release-LTO

# 2. 启动 Ninja 后端进行多核极限编译与 LTO 链接
cmake --build out/build/Release-LTO
```

首次配置会从源码编译全部依赖（libjxl、libavif、LibRaw 等），耗时较长；之后的配置会复用 vcpkg 二进制缓存。

编译产物位于 `out/build/Release-LTO/QuickView.exe`。

> **其他预设**：`Debug-ASan`（AddressSanitizer，关闭 LTO）与 `ARM64-Release-LTO`（ARM64 + Full LTO）。构建目录始终与预设同名，例如 `out/build/Debug-ASan`。

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
