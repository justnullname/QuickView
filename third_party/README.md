# QuickView Third-Party Dependencies

This directory manages external dependencies using Git submodules for easy synchronization with upstream repositories.

## Submodules

| Library | Repository | Version | Purpose |
|---------|------------|---------|---------|
| libjpeg-turbo | https://github.com/libjpeg-turbo/libjpeg-turbo | latest | Lossless JPEG transforms |

## Setup

### Initial Clone (with submodules)

```powershell
git clone --recurse-submodules https://github.com/user/QuickView.git
```

### Add Submodule (first time)

```powershell
# Add libjpeg-turbo
git submodule add https://github.com/libjpeg-turbo/libjpeg-turbo.git third_party/libjpeg-turbo
git commit -m "Add libjpeg-turbo submodule"
```

### Update Submodules

```powershell
# Update all submodules to latest
git submodule update --remote --merge

# Or update specific submodule
cd third_party/libjpeg-turbo
git pull origin main
cd ../..
git add third_party/libjpeg-turbo
git commit -m "Update libjpeg-turbo to latest"
```

### After Pulling (sync submodules)

```powershell
git submodule update --init --recursive
```

## Building Dependencies

### libjpeg-turbo

```powershell
cd third_party/libjpeg-turbo

# Create build directory
mkdir build
cd build

# Configure (x64 Release)
cmake .. -G "Visual Studio 17 2022" -A x64 -DCMAKE_BUILD_TYPE=Release

# Build
cmake --build . --config Release

# Output: build/Release/turbojpeg-static.lib
```

### Copy to Project

After building, copy the required files:

```powershell
# Headers
Copy-Item "third_party/libjpeg-turbo/*.h" "QuickView_VS2026_X64/src/QuickView2026/include/libjpeg-turbo/"

# Libraries
Copy-Item "third_party/libjpeg-turbo/build/Release/turbojpeg-static.lib" "QuickView_VS2026_X64/src/QuickView2026/lib/x64/"
```

## Directory Structure

```
third_party/
├── README.md           (this file)
├── libjpeg-turbo/      (git submodule)
│   ├── CMakeLists.txt
│   ├── turbojpeg.h
│   └── ...
└── [future libraries]/
```

## Adding New Dependencies

1. Add as submodule:
   ```powershell
   git submodule add <repo-url> third_party/<name>
   ```

2. Update this README with build instructions

3. Add to project's include/library paths
