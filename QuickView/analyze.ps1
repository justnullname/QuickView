$files = "debug_log_part1.txt", "debug_log_part2a.txt", "debug_log_part2b.txt", "debug_log_part3a.txt", "debug_log_part3b.txt"
$dispatched = @{}
$ready = @{}

Write-Host "Reading logs..."
foreach ($f in $files) {
    if (Test-Path $f) {
        $content = Get-Content $f
        foreach ($line in $content) {
            # Regex for "Decode Tile (LOD L, C c R r)"
            if ($line -match "Decode Tile \(LOD (\d+), C(\d+) R(\d+)\)") {
                $lod = $matches[1]
                $x = $matches[2]
                $y = $matches[3]
                $key = "$lod,$x,$y"
                if (-not $dispatched.ContainsKey($key)) { $dispatched[$key] = $true }
            }
            # Regex for "OnTileReady: ... (LOD=L X=x Y=y)"
            if ($line -match "OnTileReady: .*\(LOD=(\d+) X=(\d+) Y=(\d+)\)") {
                $lod = $matches[1]
                $x = $matches[2]
                $y = $matches[3]
                $key = "$lod,$x,$y"
                if (-not $ready.ContainsKey($key)) { $ready[$key] = $true }
            }
        }
    }
}

Write-Host "Checking for Stuck Tiles..."
$stuckCount = 0
foreach ($k in $dispatched.Keys) {
    if (-not $ready.ContainsKey($k)) {
        Write-Host "STUCK (Dispatched but not Ready): $k"
        $stuckCount++
    }
}
if ($stuckCount -eq 0) { Write-Host "No stuck tiles found." }

Write-Host "`nChecking for Missing Tiles in LOD 0 Grid..."
$lod0_xs = @()
$lod0_ys = @()
foreach ($k in $ready.Keys) {
    $p = $k.Split(",")
    if ($p[0] -eq "0") {
        $lod0_xs += [int]$p[1]
        $lod0_ys += [int]$p[2]
    }
}

if ($lod0_xs.Count -gt 0) {
    $minX = ($lod0_xs | Measure-Object -Minimum).Minimum
    $maxX = ($lod0_xs | Measure-Object -Maximum).Maximum
    $minY = ($lod0_ys | Measure-Object -Minimum).Minimum
    $maxY = ($lod0_ys | Measure-Object -Maximum).Maximum
    
    Write-Host "LOD 0 Bounds: X[$minX..$maxX] Y[$minY..$maxY]"
    
    $missingCount = 0
    for ($y = $minY; $y -le $maxY; $y++) {
        for ($x = $minX; $x -le $maxX; $x++) {
            $k = "0,$x,$y"
            # CHECK READY instead of Dispatched
            if (-not $ready.ContainsKey($k)) {
                # Heuristic: If neighbors exist, it's likely a gap
                $hasL = $ready.ContainsKey("0,$($x-1),$y")
                $hasR = $ready.ContainsKey("0,$($x+1),$y")
                $hasT = $ready.ContainsKey("0,$x,$($y-1)")
                $hasB = $ready.ContainsKey("0,$x,$($y+1)")
                $neighbors = 0
                if ($hasL) { $neighbors++ }
                if ($hasR) { $neighbors++ }
                if ($hasT) { $neighbors++ }
                if ($hasB) { $neighbors++ }
               
                if ($neighbors -ge 2) {
                    Write-Host "MISSING TILE: LOD 0 ($x, $y) (Surrounded by $neighbors neighbors)"
                    $missingCount++
                }
            }
        }
    }
    if ($missingCount -eq 0) { Write-Host "No obvious gaps in LOD 0 grid (via OnTileReady)." }
}
else {
    Write-Host "No LOD 0 tiles found in log."
}
