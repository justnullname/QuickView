param(
    [string]$SourceImage = (Join-Path $PSScriptRoot "..\..\static\Logo\QuickViewLogo.svg"),
    [string]$OutputDir = (Join-Path $PSScriptRoot "Assets"),
    [switch]$UpdateIco,
    [string]$IcoOutputPath = (Join-Path $PSScriptRoot "..\..\QuickView.ico")
)

$ErrorActionPreference = "Stop"
Add-Type -AssemblyName System.Drawing

if (-not (Test-Path $SourceImage)) {
    throw "Source image not found: $SourceImage"
}

$resolvedSource = (Resolve-Path $SourceImage).Path
New-Item -ItemType Directory -Force -Path $OutputDir | Out-Null
$resolvedOutputDir = (Resolve-Path $OutputDir).Path

function Find-EdgeExecutable {
    $candidates = @(
        "${env:ProgramFiles(x86)}\Microsoft\Edge\Application\msedge.exe",
        "$env:ProgramFiles\Microsoft\Edge\Application\msedge.exe"
    )
    foreach ($cmdName in @("msedge.exe", "msedge")) {
        $cmd = Get-Command $cmdName -ErrorAction SilentlyContinue
        if ($cmd -and $cmd.Source) {
            $candidates = @($cmd.Source) + $candidates
        }
    }
    foreach ($p in $candidates) {
        if (-not [string]::IsNullOrWhiteSpace($p) -and (Test-Path $p)) {
            return $p
        }
    }
    return $null
}

function Find-PythonWithPillow {
    foreach ($pyName in @("python", "python3", "py")) {
        $cmd = Get-Command $pyName -ErrorAction SilentlyContinue
        if ($cmd -and $cmd.Source) {
            $args = if ($pyName -eq "py") { @("-3", "-c", "import PIL; print('OK')") } else { @("-c", "import PIL; print('OK')") }
            $out = & $cmd.Source @args 2>$null
            if ($LASTEXITCODE -eq 0 -and ($out -join "") -match "OK") {
                return $pyName
            }
        }
    }
    return $null
}

function Render-SvgMasterPng {
    param(
        [string]$SvgPath,
        [string]$OutPngPath,
        [int]$MasterSize = 2048
    )

    $edgeExe = Find-EdgeExecutable
    if (-not $edgeExe) {
        return $false
    }

    $tempDir = Join-Path $env:TEMP ("QuickView_SvgRaster_" + [guid]::NewGuid().ToString("N"))
    $profileDir = Join-Path $tempDir "edge_profile"
    New-Item -ItemType Directory -Force -Path $profileDir | Out-Null

    try {
        $svgBytes = [System.IO.File]::ReadAllBytes($SvgPath)
        $svgB64 = [Convert]::ToBase64String($svgBytes)
        $html = @"
<!DOCTYPE html>
<html>
<head>
<meta charset="utf-8">
<style>
  html, body {
    margin: 0;
    padding: 0;
    width: 100%;
    height: 100%;
    background: transparent;
    overflow: hidden;
  }
  img {
    display: block;
    width: ${MasterSize}px;
    height: ${MasterSize}px;
  }
</style>
</head>
<body>
  <img src="data:image/svg+xml;base64,$svgB64" />
</body>
</html>
"@
        $htmlPath = Join-Path $tempDir "render.html"
        [System.IO.File]::WriteAllText($htmlPath, $html, [System.Text.UTF8Encoding]::new($false))
        $htmlUri = [System.Uri]::new($htmlPath).AbsoluteUri

        $edgeArgs = @(
            "--headless=new",
            "--disable-gpu",
            "--no-first-run",
            "--no-default-browser-check",
            "--hide-scrollbars",
            "--default-background-color=00000000",
            "--window-size=$MasterSize,$MasterSize",
            "--force-device-scale-factor=1",
            "--user-data-dir=$profileDir",
            "--screenshot=$OutPngPath",
            $htmlUri
        )

        $proc = Start-Process -FilePath $edgeExe -ArgumentList $edgeArgs -Wait -PassThru -NoNewWindow
        if ($proc.ExitCode -ne 0 -or -not (Test-Path $OutPngPath)) {
            return $false
        }

        $bmp = [System.Drawing.Bitmap]::FromFile($OutPngPath)
        try {
            $corner = $bmp.GetPixel(0, 0)
            if ($bmp.Width -ne $MasterSize -or $bmp.Height -ne $MasterSize -or $corner.A -ne 0) {
                Write-Warning "Edge SVG rasterization validation failed (${($bmp.Width)}x${($bmp.Height)}, corner alpha=$($corner.A))."
                return $false
            }
        } finally {
            $bmp.Dispose()
        }
        return $true
    } finally {
        Remove-Item -Path $tempDir -Recurse -Force -ErrorAction SilentlyContinue
    }
}

$workTempDir = Join-Path $env:TEMP ("QuickView_AssetsGen_" + [guid]::NewGuid().ToString("N"))
New-Item -ItemType Directory -Force -Path $workTempDir | Out-Null

try {
    $masterPngPath = $resolvedSource
    if ([System.IO.Path]::GetExtension($resolvedSource).ToLowerInvariant() -eq ".svg") {
        $svgMasterOut = Join-Path $workTempDir "master_2048.png"
        Write-Host "[Assets] Rasterizing vector SVG via headless Edge: $resolvedSource -> 2048x2048 PNG"
        if (Render-SvgMasterPng -SvgPath $resolvedSource -OutPngPath $svgMasterOut -MasterSize 2048) {
            $masterPngPath = $svgMasterOut
        } else {
            $fallbackPng = Join-Path $PSScriptRoot "..\..\QuickView_favicon_large.png"
            if (Test-Path $fallbackPng) {
                Write-Warning "SVG rasterization via Edge unavailable; falling back to $fallbackPng"
                $masterPngPath = (Resolve-Path $fallbackPng).Path
            } else {
                throw "Failed to rasterize SVG ($resolvedSource) and fallback PNG was not found."
            }
        }
    }

    # Build complete MSIX asset manifest (no bare un-qualified files to avoid PRI duplicate conflicts).
    $assetSpecs = [System.Collections.Generic.List[hashtable]]::new()

    $scaleFactors = @(
        @{ tag = "scale-100"; pct = 100 },
        @{ tag = "scale-125"; pct = 125 },
        @{ tag = "scale-150"; pct = 150 },
        @{ tag = "scale-200"; pct = 200 },
        @{ tag = "scale-400"; pct = 400 }
    )

    foreach ($sf in $scaleFactors) {
        $s50  = [int][Math]::Round(50.0  * $sf.pct / 100.0)
        $s44  = [int][Math]::Round(44.0  * $sf.pct / 100.0)
        $s150 = [int][Math]::Round(150.0 * $sf.pct / 100.0)
        $s310 = [int][Math]::Round(310.0 * $sf.pct / 100.0)
        $wW   = [int][Math]::Round(310.0 * $sf.pct / 100.0)
        $wH   = [int][Math]::Round(150.0 * $sf.pct / 100.0)
        $wIco = [int][Math]::Round(120.0 * $sf.pct / 100.0)

        $assetSpecs.Add(@{ file = "StoreLogo.$($sf.tag).png"; w = $s50; h = $s50; iw = $s50; ih = $s50 })
        $assetSpecs.Add(@{ file = "Square44x44Logo.$($sf.tag).png"; w = $s44; h = $s44; iw = $s44; ih = $s44 })
        $assetSpecs.Add(@{ file = "Square150x150Logo.$($sf.tag).png"; w = $s150; h = $s150; iw = $s150; ih = $s150 })
        $assetSpecs.Add(@{ file = "Wide310x150Logo.$($sf.tag).png"; w = $wW; h = $wH; iw = $wIco; ih = $wIco })
        $assetSpecs.Add(@{ file = "Square310x310Logo.$($sf.tag).png"; w = $s310; h = $s310; iw = $s310; ih = $s310 })
    }

    $targetSizes = @(16, 20, 24, 30, 32, 36, 40, 48, 60, 64, 72, 80, 96, 256)
    foreach ($ts in $targetSizes) {
        $assetSpecs.Add(@{ file = "Square44x44Logo.targetsize-$ts.png"; w = $ts; h = $ts; iw = $ts; ih = $ts })
        $assetSpecs.Add(@{ file = "Square44x44Logo.targetsize-${ts}_altform-unplated.png"; w = $ts; h = $ts; iw = $ts; ih = $ts })
        $assetSpecs.Add(@{ file = "Square44x44Logo.targetsize-${ts}_altform-lightunplated.png"; w = $ts; h = $ts; iw = $ts; ih = $ts })
    }

    # Remove stale PNG files first so qualified resources cleanly replace bare legacy names.
    Get-ChildItem -Path $resolvedOutputDir -Filter "*.png" -ErrorAction SilentlyContinue | Remove-Item -Force

    $icoSizes = @(16, 20, 24, 30, 32, 36, 40, 48, 60, 64, 72, 96, 256)
    $pythonCmd = Find-PythonWithPillow

    if ($pythonCmd) {
        Write-Host "[Assets] Downsampling via Python Pillow (premultiplied RGBa + Lanczos)..."
        $manifestObj = @{
            master = $masterPngPath
            outputDir = $resolvedOutputDir
            assets = $assetSpecs
            updateIco = [bool]$UpdateIco
            icoPath = $IcoOutputPath
            icoSizes = $icoSizes
        }
        $jobJsonPath = Join-Path $workTempDir "job.json"
        $pyScriptPath = Join-Path $workTempDir "render_assets.py"
        [System.IO.File]::WriteAllText($jobJsonPath, ($manifestObj | ConvertTo-Json -Depth 5), [System.Text.UTF8Encoding]::new($false))

        $pyCode = @'
import io
import json
import os
import struct
import sys
from PIL import Image

def resize_premul(src_premul: Image.Image, w: int, h: int) -> Image.Image:
    if src_premul.size == (w, h):
        return src_premul.convert("RGBA")
    return src_premul.resize((w, h), Image.Resampling.LANCZOS).convert("RGBA")

def build_dib_frame(rgba: Image.Image) -> bytes:
    w, h = rgba.size
    # Flip vertically for bottom-up DIB and convert RGBA -> BGRA.
    flipped = rgba.transpose(Image.Transpose.FLIP_TOP_BOTTOM)
    r, g, b, a = flipped.split()
    bgra_bytes = Image.merge("RGBA", (b, g, r, a)).tobytes()

    bih = struct.pack(
        "<IiiHHIIiiII",
        40,        # biSize
        w,         # biWidth
        h * 2,     # biHeight (XOR + AND)
        1,         # biPlanes
        32,        # biBitCount
        0,         # biCompression (BI_RGB)
        len(bgra_bytes),
        0, 0, 0, 0
    )
    and_row_stride = ((w + 31) // 32) * 4
    and_mask = b"\x00" * (and_row_stride * h)
    return bih + bgra_bytes + and_mask

def build_png_frame(rgba: Image.Image) -> bytes:
    buf = io.BytesIO()
    rgba.save(buf, format="PNG", optimize=True)
    return buf.getvalue()

def write_ico(src_premul: Image.Image, ico_path: str, sizes: list[int]) -> None:
    frames: list[tuple[int, bytes]] = []
    for s in sizes:
        img = resize_premul(src_premul, s, s)
        payload = build_dib_frame(img) if s <= 48 else build_png_frame(img)
        frames.append((s, payload))

    count = len(frames)
    header = struct.pack("<HHH", 0, 1, count)
    offset = 6 + count * 16
    entries = []
    blobs = []
    for s, payload in frames:
        dim = 0 if s >= 256 else s
        entries.append(struct.pack("<BBBBHHII", dim, dim, 0, 0, 1, 32, len(payload), offset))
        blobs.append(payload)
        offset += len(payload)

    with open(ico_path, "wb") as f:
        f.write(header)
        for e in entries:
            f.write(e)
        for b in blobs:
            f.write(b)

def main() -> None:
    with open(sys.argv[1], "r", encoding="utf-8") as f:
        cfg = json.load(f)

    src = Image.open(cfg["master"]).convert("RGBA")
    src_premul = src.convert("RGBa")
    out_dir = cfg["outputDir"]

    for item in cfg["assets"]:
        cw, ch = int(item["w"]), int(item["h"])
        iw, ih = int(item["iw"]), int(item["ih"])
        icon_img = resize_premul(src_premul, iw, ih)
        if cw == iw and ch == ih:
            out_img = icon_img
        else:
            canvas = Image.new("RGBA", (cw, ch), (0, 0, 0, 0))
            ox = (cw - iw) // 2
            oy = (ch - ih) // 2
            canvas.alpha_composite(icon_img, (ox, oy))
            out_img = canvas
        out_img.save(os.path.join(out_dir, item["file"]), format="PNG", optimize=True)

    if cfg.get("updateIco"):
        sizes_desc = sorted([int(s) for s in cfg["icoSizes"]], reverse=True)
        write_ico(src_premul, cfg["icoPath"], sizes_desc)
        print(f"Updated ICO: {cfg['icoPath']} with {len(sizes_desc)} sizes ({sizes_desc})")

    print(f"Generated {len(cfg['assets'])} MSIX PNG assets in {out_dir}")

if __name__ == "__main__":
    main()
'@
        [System.IO.File]::WriteAllText($pyScriptPath, $pyCode, [System.Text.UTF8Encoding]::new($false))
        $pyRunArgs = if ($pythonCmd -eq "py") { @("-3", $pyScriptPath, $jobJsonPath) } else { @($pyScriptPath, $jobJsonPath) }
        & $pythonCmd @pyRunArgs
        if ($LASTEXITCODE -ne 0) {
            throw "Python Pillow asset generator failed with exit code $LASTEXITCODE"
        }
    } else {
        Write-Warning "Python Pillow not available; using System.Drawing progressive downsampling fallback."
        if ($UpdateIco) {
            throw "Python Pillow is required when -UpdateIco is specified."
        }
        $srcImg = [System.Drawing.Image]::FromFile($masterPngPath)
        try {
            foreach ($spec in $assetSpecs) {
                $cw = [int]$spec.w
                $ch = [int]$spec.h
                $iw = [int]$spec.iw
                $ih = [int]$spec.ih
                $bmp = New-Object System.Drawing.Bitmap($cw, $ch, [System.Drawing.Imaging.PixelFormat]::Format32bppArgb)
                $g = [System.Drawing.Graphics]::FromImage($bmp)
                $g.Clear([System.Drawing.Color]::Transparent)
                $g.InterpolationMode = [System.Drawing.Drawing2D.InterpolationMode]::HighQualityBicubic
                $g.SmoothingMode = [System.Drawing.Drawing2D.SmoothingMode]::HighQuality
                $g.PixelOffsetMode = [System.Drawing.Drawing2D.PixelOffsetMode]::HighQuality
                $g.CompositingQuality = [System.Drawing.Drawing2D.CompositingQuality]::HighQuality
                $ox = [int](($cw - $iw) / 2)
                $oy = [int](($ch - $ih) / 2)
                $g.DrawImage($srcImg, $ox, $oy, $iw, $ih)
                $g.Dispose()
                $bmp.Save((Join-Path $resolvedOutputDir $spec.file), [System.Drawing.Imaging.ImageFormat]::Png)
                $bmp.Dispose()
            }
            Write-Host "Generated $($assetSpecs.Count) MSIX PNG assets via System.Drawing."
        } finally {
            $srcImg.Dispose()
        }
    }
} finally {
    Remove-Item -Path $workTempDir -Recurse -Force -ErrorAction SilentlyContinue
}
