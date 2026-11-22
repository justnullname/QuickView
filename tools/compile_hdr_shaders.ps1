param()

$ErrorActionPreference = 'Stop'

$repoRoot = Split-Path -Parent $PSScriptRoot
$shaderDir = Join-Path $repoRoot 'QuickView\shaders'
$headerPath = Join-Path $repoRoot 'QuickView\CompiledComputeShaders.h'

$fxcCandidates = Get-ChildItem 'C:\Program Files (x86)\Windows Kits\10\bin\*\x64\fxc.exe' -ErrorAction SilentlyContinue |
    Sort-Object { [version](Split-Path (Split-Path (Split-Path $_.FullName)) -Leaf) } -Descending
if (-not $fxcCandidates -or $fxcCandidates.Count -eq 0) {
    throw 'fxc.exe not found under Windows Kits 10.'
}
$fxc = $fxcCandidates[0].FullName
Write-Host "Using fxc: $fxc"

$tempDir = Join-Path $env:TEMP ('qv_shader_build_' + [guid]::NewGuid().ToString('N'))
New-Item -ItemType Directory -Path $tempDir -Force | Out-Null

function Format-ShaderBlock {
    param(
        [string]$DisplayName,
        [string]$EntryPoint,
        [string]$SymbolName,
        [string]$HlslFile
    )

    $hlslPath = Join-Path $shaderDir $HlslFile
    $csoPath = Join-Path $tempDir "$SymbolName.cso"
    $asmPath = Join-Path $tempDir "$SymbolName.asm"

    $fxcOutput = & $fxc /nologo /T cs_5_0 /E $EntryPoint /O3 /Qstrip_reflect /Qstrip_debug /Qstrip_priv /I $shaderDir /Fo $csoPath /Fc $asmPath $hlslPath 2>&1
    if ($LASTEXITCODE -ne 0) {
        $fxcOutput | Write-Host
        throw "fxc compilation failed for $HlslFile ($EntryPoint)"
    }
    foreach ($line in $fxcOutput) {
        if ($line -match 'warning|error') {
            Write-Host $line
        }
    }

    [byte[]]$bytes = [System.IO.File]::ReadAllBytes($csoPath)
    $asmLines = [System.IO.File]::ReadAllLines($asmPath)

    $sb = [System.Text.StringBuilder]::new()
    [void]$sb.AppendLine("// --- $DisplayName ($EntryPoint, cs_5_0, stripped) ---")
    [void]$sb.AppendLine("#if 0")
    foreach ($line in $asmLines) {
        [void]$sb.AppendLine("// $line")
    }
    [void]$sb.AppendLine("#endif")
    [void]$sb.AppendLine("")
    [void]$sb.AppendLine("inline constexpr uint8_t ${SymbolName}[] =")
    [void]$sb.AppendLine("{")

    for ($i = 0; $i -lt $bytes.Length; $i += 6) {
        $end = [Math]::Min($i + 6, $bytes.Length)
        $chunk = for ($j = $i; $j -lt $end; $j++) {
            '{0,3}' -f $bytes[$j]
        }
        $lineText = '    ' + ($chunk -join ', ')
        if ($end -lt $bytes.Length) {
            $lineText += ', '
        }
        [void]$sb.AppendLine($lineText)
    }

    [void]$sb.AppendLine("};")
    return $sb.ToString()
}

try {
    $blockToneMapSdr = Format-ShaderBlock -DisplayName 'ToneMapHdrToSdr' -EntryPoint 'CSToneMap' -SymbolName 'g_csToneMapHdrToSdr' -HlslFile 'ToneMapHdrToSdr.hlsl'
    $blockToneMapHdr = Format-ShaderBlock -DisplayName 'ToneMapHdrToHdr' -EntryPoint 'CSToneMapHDR' -SymbolName 'g_csToneMapHdrToHdr' -HlslFile 'ToneMapHdrToHdr.hlsl'
    $blockCompose = Format-ShaderBlock -DisplayName 'ComposeGainMap' -EntryPoint 'CSComposeGainMap' -SymbolName 'g_csComposeGainMap' -HlslFile 'ComposeGainMap.hlsl'

    $headerText = [System.IO.File]::ReadAllText($headerPath, [System.Text.Encoding]::UTF8)
    $startMarker = '// --- ToneMapHdrToSdr (CSToneMap, cs_5_0, stripped) ---'
    $endMarker = '// --- FsrEasu (CSFSR_EASU, cs_5_0, stripped) ---'

    $startIdx = $headerText.IndexOf($startMarker, [System.StringComparison]::Ordinal)
    $endIdx = $headerText.IndexOf($endMarker, [System.StringComparison]::Ordinal)
    if ($startIdx -lt 0 -or $endIdx -le $startIdx) {
        throw "Could not locate shader section markers in $headerPath"
    }

    $replacement = $blockToneMapSdr + "`r`n" + $blockToneMapHdr + "`r`n" + $blockCompose + "`r`n"
    $updated = $headerText.Substring(0, $startIdx) + $replacement + $headerText.Substring($endIdx)
    $utf8NoBom = [System.Text.UTF8Encoding]::new($false)
    [System.IO.File]::WriteAllText($headerPath, $updated, $utf8NoBom)

    Write-Host "Updated $headerPath successfully."
}
finally {
    if (Test-Path $tempDir) {
        Remove-Item -Path $tempDir -Recurse -Force -ErrorAction SilentlyContinue
    }
}
