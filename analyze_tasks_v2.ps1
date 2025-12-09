$dir = "C:\Users\vivoo\.gemini\antigravity\brain\13d1aea8-1d73-4bd3-bc00-b87101acc948"
$files = Get-ChildItem -Path $dir -Filter "task.md.resolved.*" | Sort-Object LastWriteTime
$uniqueTasks = @{}

foreach ($file in $files) {
    try {
        $content = Get-Content $file.FullName
        foreach ($line in $content) {
            # Match list items
            if ($line -match "^\s*- \[( |x|/)\] (.*)") {
                $status = $matches[1].Trim() # "" or "x" or "/"
                $taskRaw = $matches[2].Trim()
                $cleanTask = $taskRaw -replace " <!-- id: .* -->", ""
                
                # Filter out headers formatted as tasks
                if ($cleanTask -match "^\*\*.*\(v\d+\.\d+\.\d+\)\*\*$") { continue }
                if ($cleanTask -match "^Phase \d+:") { continue } # We recovered these already
                
                if (-not $uniqueTasks.ContainsKey($cleanTask)) {
                    $uniqueTasks[$cleanTask] = $status
                }
                else {
                    # If we see it marked as 'x' later, update status? 
                    # Actually, if it was ANYWHERE marked as ' ' (open), we might care.
                    # But the user wants "Development Ideas", so even completed ones might be interesting if we missed them in history.
                    # Let's keep the *latest* status found? No, files are sorted by time.
                    # So the last file processing will overwrite status.
                    $uniqueTasks[$cleanTask] = $status
                }
            }
        }
    }
    catch {}
}

# Current tasks
$currentFile = "$dir\task.md"
$currentTaskMap = @{}
if (Test-Path $currentFile) {
    Get-Content $currentFile | ForEach-Object {
        if ($_ -match "^\s*- \[( |x|/)\] (.*)") {
            $t = $matches[2].Trim() -replace " <!-- id: .* -->", ""
            $currentTaskMap[$t] = $true
        }
    }
}

$outputFile = "e:\Personal\Coding\QuickView\missing_tasks.txt"
$writer = [System.IO.StreamWriter]::new($outputFile)

$writer.WriteLine("--- RECOVERED UNTRACKED TASKS ---")
foreach ($key in $uniqueTasks.Keys) {
    if (-not $currentTaskMap.ContainsKey($key)) {
        $status = $uniqueTasks[$key]
        $mark = if ($status -eq "x") { "[x]" } else { "[ ]" }
        $writer.WriteLine("- $mark $key")
    }
}
$writer.Close()

Write-Output "Results written to $outputFile"
