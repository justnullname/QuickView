# QuickView v3.0.4 - The Quantum Flow Update
**Release Date**: 2026-01-16

### ⚡ Core Architecture: "Quantum Flow"
- **Unified Scheduling & Decoding (Quantum Flow)**: Introduced a "Fast/Slow Dual-Channel" architecture (`FastLane` + `HeavyLanePool`) that isolates instant interactions from heavy decoding tasks.
- **N+1 Hot-Spare Architecture**: Implemented a "Capped N+1" threading model where standby threads are kept warm for immediate response, maximizing CPU throughput without over-subscription.
- **Deep Cancellation**: Granular "On-Demand" cancellation logic allowed for heavy formats (JXL/RAW/WebP), ensuring stale tasks (e.g., during rapid scrolling) are instantly terminated to save power.
- **Direct D2D Passthrough**: Established a "Zero-Copy" pipeline where decoded `RawImageFrame` buffers are uploaded directly to GPU memory, bypassing GDI/GDI+ entirely.

### 🎨 Visual & Rendering Refactor
- **DirectComposition (Game-Grade Rendering)**: Completely abandoned the legacy SwapChain/GDI model in favor of a `DirectComposition` Visual tree.
    - **Visual Ping-Pong**: Implemented a double-buffered Visual architecture for tear-free, artifact-free crossfades.
    - **IDCompositionScaleTransform**: Hardware-accelerated high-precision zooming and panning.
- **Native SVG Engine**: Replaced `nanosvg` with **Direct2D Native SVG** rendering.
    - **Capabilities**: Supports complex SVG filters, gradients, and CSS transparency.
    - **2-Stage Lossless Scaling**: Vector-based re-rasterization during deep zoom for infinite sharpness.
    - *(Requirement: Windows 10 Creators Update 1703 or later)*.

### 💾 Memory & Resource Management
- **Arena Dynamic Allocation**: Switched to a **TripleArena** strategy using Polymorphic Memory Resources (PMR). Memory is pre-allocated and recycled (Bucket Strategy) to eliminate heap fragmentation.
- **Smart Directional Prefetch**:
    - **Auto-Tuning**: Automatically selects `Eco`, `Balanced`, or `Performance` prefetch strategies based on detected system RAM.
    - **Manual Override**: Full user control over cache behavior.
    - **Smart Skip**: Prevents "OOM" in Eco mode by intelligently skipping tasks that exceed the cache budget.

### 🧩 Infrastructure & Metadata
- **Metadata Architecture Refactor**: Decoupled "Fast Header Peeking" (for instant layout) from "Async Rich Metadata" parsing (Exif/IPTC/XMP), solving UI blocking issues.
- **Debug HUD**: Added a real-time "Matrix" overlay (`F12`) visualizing the topology of the cache, worker lane status, and frame timings.


