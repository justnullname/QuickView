$content = Get-Content "e:\Personal\Coding\QuickView\QuickView_VS2026_X64\src\JPEGView\MainDlg.cpp"
$balance = 0
$lineNum = 0
foreach ($line in $content) {
    $lineNum++
    if ($line.Length -gt 0) {
        $code = $line.Split("//")[0]
        foreach ($char in $code.ToCharArray()) {
            if ($char -eq '{') { $balance++ }
            if ($char -eq '}') { $balance-- }
        }
    }
    if ($balance -lt 0) {
        Write-Host "Extra closing brace at line $lineNum"
        break
    }
}
Write-Host "Final balance: $balance"
