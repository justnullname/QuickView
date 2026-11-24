[CmdletBinding(DefaultParameterSetName = "PackSingle")]
param(
    [Parameter(ParameterSetName = "PackSingle", Mandatory = $true)]
    [ValidateSet("x64", "arm64")]
    [string]$Arch,

    [Parameter(ParameterSetName = "PackSingle")]
    [string]$BuildDir,

    [Parameter(ParameterSetName = "PackSingle")]
    [string]$OutputPath,

    [Parameter(ParameterSetName = "Bundle", Mandatory = $true)]
    [switch]$CreateBundle,

    [Parameter(ParameterSetName = "Bundle", Mandatory = $true)]
    [string[]]$MsixInputs,

    [Parameter(ParameterSetName = "Bundle", Mandatory = $true)]
    [string]$BundleOutputPath,

    [string]$Version,

    [switch]$TestSign
)

$ErrorActionPreference = "Stop"
Add-Type -AssemblyName System.IO.Compression.FileSystem

$RepoRoot = (Resolve-Path (Join-Path $PSScriptRoot "..\..")).Path
$RcPath = Join-Path $RepoRoot "QuickView\QuickView.rc"
$ExtHeaderPath = Join-Path $RepoRoot "QuickView\SupportedExtensions.h"
$ManifestTemplatePath = Join-Path $PSScriptRoot "AppxManifest.xml.in"
$AssetsDir = Join-Path $PSScriptRoot "Assets"

function Find-SdkTool {
    param([string]$ToolName)
    $kitsRoot = "${env:ProgramFiles(x86)}\Windows Kits\10\bin"
    if (-not (Test-Path $kitsRoot)) {
        throw "Windows 10/11 SDK not found under $kitsRoot"
    }
    $archOrder = @("x64", "arm64", "x86")
    $versionDirs = Get-ChildItem -Path $kitsRoot -Directory |
        Where-Object { $_.Name -match '^\d+\.\d+\.\d+\.\d+$' } |
        Sort-Object { [version]$_.Name } -Descending
    foreach ($vDir in $versionDirs) {
        foreach ($a in $archOrder) {
            $candidate = Join-Path $vDir.FullName "$a\$ToolName"
            if (Test-Path $candidate) {
                return $candidate
            }
        }
    }
    throw "$ToolName not found under $kitsRoot"
}

function Assert-MsixContainsPriAndUnplatedAssets {
    param([string]$MsixFilePath)
    $zip = [System.IO.Compression.ZipFile]::OpenRead((Resolve-Path $MsixFilePath).Path)
    try {
        $entryMap = @{}
        foreach ($e in $zip.Entries) {
            $entryMap[$e.FullName.ToLowerInvariant()] = $e.Length
        }
        if (-not $entryMap.ContainsKey("resources.pri") -or $entryMap["resources.pri"] -le 0) {
            throw "MSIX validation failed ($MsixFilePath): missing or empty resources.pri"
        }
        if (-not $entryMap.ContainsKey("assets/square44x44logo.targetsize-48_altform-unplated.png")) {
            throw "MSIX validation failed ($MsixFilePath): missing Assets/Square44x44Logo.targetsize-48_altform-unplated.png"
        }
    } finally {
        $zip.Dispose()
    }
}

function Get-StoreVersionFromRc {
    param([string]$RcFile)
    $rcText = Get-Content -Path $RcFile -Raw -Encoding UTF8
    if ($rcText -match 'FILEVERSION\s+(\d+)\s*,\s*(\d+)\s*,\s*(\d+)\s*,\s*(\d+)') {
        # Microsoft Store requires the 4th revision segment of MSIX version to be 0.
        return "$($Matches[1]).$($Matches[2]).$($Matches[3]).0"
    }
    throw "Could not parse FILEVERSION from $RcFile"
}

function Get-LowercaseExtensionsXml {
    param([string]$HeaderFile)
    $content = Get-Content -Path $HeaderFile -Raw -Encoding UTF8
    $arrayNames = @(
        "STANDARD_EXTENSIONS",
        "WEB_MODERN_EXTENSIONS",
        "PRO_LEGACY_EXTENSIONS",
        "CAMERA_RAW_EXTENSIONS",
        "EXTENDED_RAW_EXTENSIONS",
        "ARCHIVE_EXTENSIONS_SEG"
    )

    $exts = New-Object System.Collections.Generic.List[string]
    $seen = New-Object 'System.Collections.Generic.HashSet[string]' ([System.StringComparer]::Ordinal)

    foreach ($name in $arrayNames) {
        $pattern = [regex]::Escape($name) + '\s*=\s*\{(?<body>.*?)\};'
        $m = [regex]::Match($content, $pattern, [System.Text.RegularExpressions.RegexOptions]::Singleline)
        if (-not $m.Success) {
            throw "Failed to locate $name in $HeaderFile"
        }
        $body = $m.Groups["body"].Value
        $extMatches = [regex]::Matches($body, 'L"(\.[A-Za-z0-9]+)"')
        foreach ($em in $extMatches) {
            $extLower = $em.Groups[1].Value.ToLowerInvariant()
            if ($seen.Add($extLower)) {
                $exts.Add($extLower)
            }
        }
    }

    if ($exts.Count -ne 92) {
        throw "Expected 92 associable extensions from SupportedExtensions.h, found $($exts.Count)"
    }

    $lines = foreach ($e in $exts) {
        "              <uap:FileType>$e</uap:FileType>"
    }
    return ($lines -join "`r`n")
}

if ([string]::IsNullOrWhiteSpace($Version)) {
    $Version = Get-StoreVersionFromRc -RcFile $RcPath
} else {
    $parts = $Version.TrimStart('v').Split('.')
    if ($parts.Count -lt 3) {
        throw "Version '$Version' must have at least Major.Minor.Patch components"
    }
    $Version = "$($parts[0]).$($parts[1]).$($parts[2]).0"
}

$makeAppx = Find-SdkTool -ToolName "makeappx.exe"
$makePri = Find-SdkTool -ToolName "makepri.exe"
Write-Host "[MSIX] Using makeappx: $makeAppx"
Write-Host "[MSIX] Using makepri:  $makePri"
Write-Host "[MSIX] Package Version (Store-normalized): $Version"

if ($PSCmdlet.ParameterSetName -eq "Bundle") {
    $bundleStaging = Join-Path $env:TEMP ("QuickView_MsixBundle_Stage_" + [guid]::NewGuid().ToString("N"))
    New-Item -ItemType Directory -Force -Path $bundleStaging | Out-Null
    try {
        foreach ($inp in $MsixInputs) {
            if (-not (Test-Path $inp)) {
                throw "Input MSIX not found: $inp"
            }
            Assert-MsixContainsPriAndUnplatedAssets -MsixFilePath $inp
            Copy-Item -Path $inp -Destination $bundleStaging -Force
        }
        $outDir = Split-Path -Parent $BundleOutputPath
        if ($outDir) {
            New-Item -ItemType Directory -Force -Path $outDir | Out-Null
        }
        Write-Host "[MSIX] Bundling unsigned Store package -> $BundleOutputPath"
        & $makeAppx bundle /d $bundleStaging /p $BundleOutputPath /bv $Version /o
        if ($LASTEXITCODE -ne 0) {
            throw "makeappx bundle failed with exit code $LASTEXITCODE"
        }
        Write-Host "[MSIX] Successfully created unsigned bundle: $BundleOutputPath"
    } finally {
        Remove-Item -Path $bundleStaging -Recurse -Force -ErrorAction SilentlyContinue
    }
    exit 0
}

if ([string]::IsNullOrWhiteSpace($BuildDir)) {
    $preset = if ($Arch -eq "x64") { "Release-LTO" } else { "ARM64-Release-LTO" }
    $BuildDir = Join-Path $RepoRoot "out\build\$preset"
}
if (-not (Test-Path $BuildDir)) {
    throw "Build directory not found: $BuildDir"
}

$exePath = Join-Path $BuildDir "QuickView.exe"
$pluginPath = Join-Path $BuildDir "plugins\sr\sr_ncnn_vulkan\sr_ncnn_vulkan.qvx"
if (-not (Test-Path $exePath)) {
    throw "QuickView.exe not found: $exePath"
}
if (-not (Test-Path $pluginPath)) {
    throw "sr_ncnn_vulkan.qvx not found: $pluginPath"
}

if ([string]::IsNullOrWhiteSpace($OutputPath)) {
    $OutputPath = Join-Path $RepoRoot "Release\msix\QuickView_${Version}_${Arch}.msix"
}
$outputParent = Split-Path -Parent $OutputPath
if ($outputParent) {
    New-Item -ItemType Directory -Force -Path $outputParent | Out-Null
}

$stagingDir = Join-Path $env:TEMP ("QuickView_Msix_Stage_${Arch}_" + [guid]::NewGuid().ToString("N"))
$priTempDir = Join-Path $env:TEMP ("QuickView_Msix_PriTemp_${Arch}_" + [guid]::NewGuid().ToString("N"))
New-Item -ItemType Directory -Force -Path $stagingDir | Out-Null
New-Item -ItemType Directory -Force -Path $priTempDir | Out-Null

try {
    Copy-Item -Path $exePath -Destination (Join-Path $stagingDir "QuickView.exe") -Force

    # Bundle pre-built sr_ncnn_vulkan.qvx without large neural model weights (.bin/.param).
    $pluginDestDir = Join-Path $stagingDir "plugins\sr\sr_ncnn_vulkan"
    New-Item -ItemType Directory -Force -Path $pluginDestDir | Out-Null
    Copy-Item -Path $pluginPath -Destination (Join-Path $pluginDestDir "sr_ncnn_vulkan.qvx") -Force

    $assetsDestDir = Join-Path $stagingDir "Assets"
    New-Item -ItemType Directory -Force -Path $assetsDestDir | Out-Null
    Copy-Item -Path (Join-Path $AssetsDir "*.png") -Destination $assetsDestDir -Force

    $fileTypesXml = Get-LowercaseExtensionsXml -HeaderFile $ExtHeaderPath
    $manifestXml = Get-Content -Path $ManifestTemplatePath -Raw -Encoding UTF8
    $manifestXml = $manifestXml.Replace("@MSIX_VERSION@", $Version)
    $manifestXml = $manifestXml.Replace("@ARCH@", $Arch)
    $manifestXml = $manifestXml.Replace("@FILE_TYPES@", $fileTypesXml)
    $stagedManifestPath = Join-Path $stagingDir "AppxManifest.xml"
    [System.IO.File]::WriteAllText($stagedManifestPath, $manifestXml, [System.Text.UTF8Encoding]::new($false))

    # Generate resources.pri so MRT qualifiers (scale-*, targetsize-*, altform-unplated) take effect.
    $priConfigPath = Join-Path $priTempDir "priconfig.xml"
    $priDumpPath = Join-Path $priTempDir "pridump.xml"
    $stagedPriPath = Join-Path $stagingDir "resources.pri"

    Write-Host "[MSIX] Generating PRI config..."
    & $makePri createconfig /cf $priConfigPath /dq en-US /o
    if ($LASTEXITCODE -ne 0) {
        throw "makepri createconfig failed with exit code $LASTEXITCODE"
    }

    # Disable autoResourcePackage splitting so all scale/targetsize PNG variants stay inside a single resources.pri.
    [xml]$priCfgXml = Get-Content -Path $priConfigPath -Raw -Encoding UTF8
    $packagingNode = $priCfgXml.resources.SelectSingleNode("packaging")
    if ($null -ne $packagingNode) {
        [void]$priCfgXml.resources.RemoveChild($packagingNode)
    }
    [System.IO.File]::WriteAllText($priConfigPath, $priCfgXml.OuterXml, [System.Text.UTF8Encoding]::new($false))

    Write-Host "[MSIX] Building resources.pri..."
    & $makePri new /pr $stagingDir /cf $priConfigPath /mn $stagedManifestPath /of $stagedPriPath /in "VivorLoong.QuickView-FastFileViewer" /o
    if ($LASTEXITCODE -ne 0 -or -not (Test-Path $stagedPriPath) -or ((Get-Item $stagedPriPath).Length -le 0)) {
        throw "makepri new failed (exitCode=$LASTEXITCODE) or produced missing/empty resources.pri"
    }

    & $makePri dump /if $stagedPriPath /of $priDumpPath /dt detailed /o
    if ($LASTEXITCODE -ne 0 -or -not (Test-Path $priDumpPath)) {
        throw "makepri dump failed with exit code $LASTEXITCODE"
    }
    $priDumpContent = Get-Content -Path $priDumpPath -Raw -Encoding UTF8
    if ($priDumpContent -notmatch 'Square44x44Logo\.png' -or
        $priDumpContent -notmatch 'UNPLATED' -or
        $priDumpContent -notmatch 'LIGHTUNPLATED') {
        throw "PRI validation failed: resources.pri did not index Square44x44Logo.png UNPLATED/LIGHTUNPLATED qualifiers."
    }
    Write-Host "[MSIX] Verified resources.pri contains Square44x44Logo UNPLATED & LIGHTUNPLATED qualifiers."

    Write-Host "[MSIX] Packing unsigned $Arch package -> $OutputPath"
    & $makeAppx pack /d $stagingDir /p $OutputPath /o
    if ($LASTEXITCODE -ne 0) {
        throw "makeappx pack failed with exit code $LASTEXITCODE"
    }
    Assert-MsixContainsPriAndUnplatedAssets -MsixFilePath $OutputPath
    Write-Host "[MSIX] Created and verified unsigned MSIX: $OutputPath"

    if ($TestSign) {
        $signTool = Find-SdkTool -ToolName "signtool.exe"
        $testDir = Join-Path $env:TEMP "QuickView-MsixTestSign"
        New-Item -ItemType Directory -Force -Path $testDir | Out-Null
        $signedCopy = Join-Path $testDir ([System.IO.Path]::GetFileNameWithoutExtension($OutputPath) + "_TestSigned.msix")
        Copy-Item -Path $OutputPath -Destination $signedCopy -Force

        $subject = "CN=EBAB6D30-ACAC-4F42-85BF-BEE7E281DE04"
        $cert = Get-ChildItem Cert:\CurrentUser\My |
            Where-Object { $_.Subject -eq $subject -and $_.NotAfter -gt (Get-Date).AddDays(1) } |
            Select-Object -First 1
        if (-not $cert) {
            $cert = New-SelfSignedCertificate -Type Custom -Subject $subject -KeyUsage DigitalSignature -FriendlyName "QuickView Local MSIX Test" -CertStoreLocation "Cert:\CurrentUser\My" -TextExtension @("2.5.29.37={text}1.3.6.1.5.5.7.3.3", "2.5.29.19={text}")
        }
        & $signTool sign /fd SHA256 /sha1 $cert.Thumbprint $signedCopy
        if ($LASTEXITCODE -ne 0) {
            throw "signtool failed with exit code $LASTEXITCODE"
        }
        $cerCopy = Join-Path $testDir "QuickView_Test.cer"
        Export-Certificate -Cert $cert -FilePath $cerCopy | Out-Null
        Write-Host "[MSIX] Test-signed copy written to temp folder (keep out of release dist): $signedCopy"
        Write-Host "[MSIX] Public test certificate exported to: $cerCopy (import to LocalMachine\TrustedPeople & LocalMachine\Root to install locally)"
    }
} finally {
    Remove-Item -Path $stagingDir -Recurse -Force -ErrorAction SilentlyContinue
    Remove-Item -Path $priTempDir -Recurse -Force -ErrorAction SilentlyContinue
}
