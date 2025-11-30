# QuickView - Fast and Lightweight Image Viewer for Windows

![QuickView icon](repository-open-graph-template-out.png?raw=true)

> A modern, streamlined fork of JPEGView optimized for Windows 10/11 with enhanced features and better performance.

## ✨ Features

### Core Functionality
- **Universal Format Support**: Supports nearly ALL image formats including JPEG, PNG, GIF, WEBP, TIFF, PSD, HEIF, AVIF, JXL, and camera RAW files (NEF, CR2, CR3, ARW, DNG, etc.)
- **Lightning Fast**: Optimized for quick loading and smooth navigation
- **Basic Image Editing**: Adjust sharpness, color balance, rotation, perspective, contrast, and exposure on-the-fly
- **ICC Color Profiles**: Accurate color reproduction

### Enhanced Features (This Fork)
- **Narrow Border Mode**: Configurable thin border in borderless mode for better visual distinction
- **Smart Fullscreen**: Double-click background to exit fullscreen, image to retain default behavior
- **Improved Zoom Display**: Semi-transparent background for better readability
- **Enhanced Window Dragging**: Drag window when image fits screen
- **Cleaner Configuration**: Renamed to `QuickView.ini` for clarity

## 📥 Download

Download the latest release from the [Releases](../../releases) page.

## 🛠️ Build from Source

### Prerequisites
- Visual Studio 2019 or later
- Windows 10 SDK or later

### Build Steps
1. Clone the repository:
   ```bash
   git clone https://github.com/YourUsername/QuickView.git
   cd QuickView
   ```

2. Open solution:
   ```bash
   cd QuickView_VS2026_X64\src
   # Open QuickView.sln in Visual Studio
   ```

3. Build:
   - Select `Release` configuration
   - Select `x64` platform
   - Build Solution (Ctrl+Shift+B)

4. Output will be in:
   ```
   QuickView_VS2026_X64\src\JPEGView\bin\x64\Release\
   ```

## ⚙️ Configuration

QuickView uses `QuickView.ini` for configuration:

**Configuration locations:**
- User config (takes precedence): `%AppData%\Roaming\JPEGView\QuickView.ini`
- Global config: `<EXE path>\QuickView.ini`

### Narrow Border Mode Settings
```ini
[JPEGView]
# Enable borderless mode
WindowBorderlessOnStartup=true

# Border settings (only visible in borderless mode)
NarrowBorderWidth=2
NarrowBorderColor=128 128 128
```

## 📝 Supported Formats

**Image Formats**: JPEG, GIF, BMP, PNG, TIFF, PSD, WEBP, JXL, HEIF/HEIC, AVIF, TGA, WDP, HDP, JXR

**Camera RAW**: DNG, CRW, CR2, CR3, NEF, NRW, ARW, SR2, ORF, RW2, RAF, X3F, PEF, MRW, KDC, DCR

See [LibRaw supported cameras](https://www.libraw.org/supported-cameras) for the complete list.

## 🎯 Project Goals

This fork focuses on:
- Eliminating legacy code and improving maintainability
- Full support for modern 64-bit Windows (10/11)
- Enhanced user experience with thoughtful improvements
- Clean, modern codebase

## 📄 License

This project is licensed under the GPL-2.0 License - see the [LICENSE](LICENSE) file for details.

## 🙏 Credits

- Original JPEGView by David Kleiner
- [Sylikc's JPEGView](https://github.com/sylikc/jpegview) - upstream fork
- LibRaw, libjpeg-turbo, libwebp, and other open-source libraries

## 🔗 Links

- [Original JPEGView](https://sourceforge.net/projects/jpegview/)
- [Sylikc's Fork](https://github.com/sylikc/jpegview)
- [LibRaw](https://www.libraw.org/)
