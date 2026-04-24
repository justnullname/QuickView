# QuickView v5.3.0 - Vector UI & Interaction Update
**Release Date**: 2026-04-23

QuickView v5.3.0 focuses on improving UI consistency and refining user interaction based on community feedback.

### 🎨 Vectorized UI Icons
We have migrated the remaining UI icons to the **GeekIcon** vector engine.
- **Improved Consistency**: Icons are now rendered using Direct2D paths, ensuring they look the same across different Windows versions and DPI settings.
- **Font Dependency Removed**: The application no longer relies on specific icon fonts for its core interface.

### 🛠 Windows Integration (#168)
- **Default Photo Viewer**: You can now register QuickView as a supported viewer in Windows "Default Apps" settings.
- **Portable Mode Refinement**: Updated the UI and logic for portable mode to better handle registry cleanup and configuration storage.

### 🎥 Interaction & Animation
- **Frame Counter (#167)**: Added a basic frame index display to the animation progress bar for GIF and WebP files.
- **Hand Cursor Panning (#160)**: Added a hand cursor when dragging images that are larger than the window.
- **Thumb Wheel Support (#156)**: Added support for vertical/horizontal mouse thumb wheels.
- **Zoom Cycle**: Refined the zoom hotkey/double-click behavior to cycle through common scaling modes.

### 🌈 HDR & Color (Experimental)
- **Luminance Handling (#131)**: We have adjusted how peak luminance is detected on HDR monitors by prioritizing system-level reports. **Note**: This is an initial attempt to address "washed out" colors, and we are still evaluating its effectiveness across different hardware.

### 🤝 Acknowledgments
A special thank you to the users who helped test this release:
- **@bananakid**, **@PYCHBI**, **@lrbin50**, **@1kari-s**, **@Battler624**, and **@toxieainc** for their bug reports, technical insights, and for sharing HDR testing resources.

We appreciate your continued support in helping us refine QuickView.
