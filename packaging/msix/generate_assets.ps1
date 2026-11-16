param(
    [string]$SourceImage = (Join-Path $PSScriptRoot "..\..\QuickView_favicon_large.png"),
    [string]$OutputDir = (Join-Path $PSScriptRoot "Assets")
)

$ErrorActionPreference = "Stop"
Add-Type -AssemblyName System.Drawing

if (-not (Test-Path $SourceImage)) {
    throw "Source image not found: $SourceImage"
}

New-Item -ItemType Directory -Force -Path $OutputDir | Out-Null
$srcImg = [System.Drawing.Image]::FromFile((Resolve-Path $SourceImage).Path)

function Save-ResizedPng {
    param(
        [int]$Width,
        [int]$Height,
        [int]$IconWidth,
        [int]$IconHeight,
        [string]$FileName
    )
    $bmp = New-Object System.Drawing.Bitmap($Width, $Height, [System.Drawing.Imaging.PixelFormat]::Format32bppArgb)
    $g = [System.Drawing.Graphics]::FromImage($bmp)
    $g.Clear([System.Drawing.Color]::Transparent)
    $g.InterpolationMode = [System.Drawing.Drawing2D.InterpolationMode]::HighQualityBicubic
    $g.SmoothingMode = [System.Drawing.Drawing2D.SmoothingMode]::HighQuality
    $g.PixelOffsetMode = [System.Drawing.Drawing2D.PixelOffsetMode]::HighQuality
    $g.CompositingQuality = [System.Drawing.Drawing2D.CompositingQuality]::HighQuality

    $x = [int](($Width - $IconWidth) / 2)
    $y = [int](($Height - $IconHeight) / 2)
    $g.DrawImage($srcImg, $x, $y, $IconWidth, $IconHeight)
    $g.Dispose()

    $outPath = Join-Path $OutputDir $FileName
    $bmp.Save($outPath, [System.Drawing.Imaging.ImageFormat]::Png)
    $bmp.Dispose()
    Write-Host "Generated $FileName (${Width}x${Height})"
}

try {
    Save-ResizedPng -Width 50  -Height 50  -IconWidth 50  -IconHeight 50  -FileName "StoreLogo.png"
    Save-ResizedPng -Width 100 -Height 100 -IconWidth 100 -IconHeight 100 -FileName "StoreLogo.scale-200.png"

    Save-ResizedPng -Width 44  -Height 44  -IconWidth 44  -IconHeight 44  -FileName "Square44x44Logo.png"
    Save-ResizedPng -Width 88  -Height 88  -IconWidth 88  -IconHeight 88  -FileName "Square44x44Logo.scale-200.png"
    Save-ResizedPng -Width 48  -Height 48  -IconWidth 48  -IconHeight 48  -FileName "Square44x44Logo.targetsize-48.png"
    Save-ResizedPng -Width 48  -Height 48  -IconWidth 48  -IconHeight 48  -FileName "Square44x44Logo.targetsize-48_altform-unplated.png"
    Save-ResizedPng -Width 256 -Height 256 -IconWidth 256 -IconHeight 256 -FileName "Square44x44Logo.targetsize-256.png"
    Save-ResizedPng -Width 256 -Height 256 -IconWidth 256 -IconHeight 256 -FileName "Square44x44Logo.targetsize-256_altform-unplated.png"

    Save-ResizedPng -Width 150 -Height 150 -IconWidth 150 -IconHeight 150 -FileName "Square150x150Logo.png"
    Save-ResizedPng -Width 300 -Height 300 -IconWidth 300 -IconHeight 300 -FileName "Square150x150Logo.scale-200.png"

    Save-ResizedPng -Width 310 -Height 150 -IconWidth 120 -IconHeight 120 -FileName "Wide310x150Logo.png"
    Save-ResizedPng -Width 620 -Height 300 -IconWidth 240 -IconHeight 240 -FileName "Wide310x150Logo.scale-200.png"

    Save-ResizedPng -Width 310 -Height 310 -IconWidth 310 -IconHeight 310 -FileName "Square310x310Logo.png"
} finally {
    $srcImg.Dispose()
}
