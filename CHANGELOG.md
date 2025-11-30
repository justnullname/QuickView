# Changelog

All notable changes to QuickView will be documented in this file.

## [Unreleased]

### Added
- **Narrow Border Mode**: Configurable thin border (width and color) that appears only in borderless windowed mode
  - Settings: `NarrowBorderWidth` (0-100, default 2) and `NarrowBorderColor` (RGB, default 128 128 128)
  - Draws after all UI elements for clear visibility
  - Automatically disabled in fullscreen mode

### Changed
- **Configuration File Renamed**: `JPEGView.ini` → `QuickView.ini`
  - User config location: `%AppData%\Roaming\JPEGView\QuickView.ini`
  - Global config location: `<EXE path>\QuickView.ini`
  
- **Improved Zoom Display**:
  - Removed duplicate zoom percentage from filename area
  - Enhanced bottom-right zoom display with semi-transparent background using AlphaBlend
  - Better readability with 60% opacity background

- **Smart Fullscreen Exit**:
  - Double-click on background (not on image) to exit fullscreen mode
  - Double-click on image retains original behavior
  - More intuitive user interaction

### Fixed
- **Window Dragging**: Added ability to drag window when image fits the window size
  - Previously only worked when image was larger than window
  - Now works in both scenarios for consistent behavior
  - Fixed issue where left-click dragging was disabled in borderless mode

- **Zoom Functionality**:
  - Fixed broken zoom with mouse wheel and keyboard shortcuts
  - Fixed window resizing not triggering when zooming ("Fit Window to Image")
  - Restored "Fit Window to Image" context menu toggle functionality

- **Startup Stability**:
  - Fixed crash on startup due to uninitialized pointer
  - Fixed Access Violation related to const-correctness in settings provider

- **Code Cleanup**: Removed legacy "JPEGView" comments and updated to "QuickView"

### Technical
- Updated all internal references from JPEGView to QuickView
- Improved codebase maintainability
- Updated INI template (QuickView.ini.tpl) with new settings

---

## Previous Versions

Based on JPEGView-Static fork, which itself is based on sylikc's JPEGView fork of the original JPEGView by David Kleiner.

For older changelog entries, see the original project documentation.
