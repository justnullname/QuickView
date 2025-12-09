$dir = "C:\Users\vivoo\.gemini\antigravity\brain\13d1aea8-1d73-4bd3-bc00-b87101acc948"
$files = Get-ChildItem -Path $dir -Filter "task.md.resolved.*" | Sort-Object LastWriteTime
$allTasks = @{}

Write-Output "Scanning $($files.Count) files..."

foreach ($file in $files) {
    try {
        $content = Get-Content $file.FullName
        foreach ($line in $content) {
            # Match list items: - [ ] Task name
            if ($line -match "^\s*- \[( |x|/)\] (.*)") {
                $taskRaw = $matches[2].Trim()
                # Remove ID comments
                $cleanTask = $taskRaw -replace " <!-- id: .* -->", ""
                # Use a dictionary to store unique tasks. 
                # We store the *original full line* (without status) as value to preserve context if needed, 
                # but key is cleaned text to dedup.
                if (-not $allTasks.ContainsKey($cleanTask)) {
                    $allTasks[$cleanTask] = $file.Name 
                }
            }
        }
    } catch {
        Write-Warning "Could not read $($file.Name)"
    }
}

# Load current task.md
$currentFile = "$dir\task.md"
$currentTasks = @{}
if (Test-Path $currentFile) {
    $currentContent = Get-Content $currentFile
    foreach ($line in $currentContent) {
        if ($line -match "^\s*- \[( |x|/)\] (.*)") {
            $taskRaw = $matches[2].Trim()
            $cleanTask = $taskRaw -replace " <!-- id: .* -->", ""
            $currentTasks[$cleanTask] = $true
        }
    }
}

Write-Output "`n--- POTENTIALLY MISSING TASKS ---"
$missingCount = 0
foreach ($key in $allTasks.Keys) {
    if (-not $currentTasks.ContainsKey($key)) {
        # Filter out generic or structural tasks that might just be headers disguised as tasks
        if ($key.Length -gt 5) {
             # Write-Output "[$($allTasks[$key])] $key"
             Write-Output "$key"
             $missingCount++
        }
    }
}

Write-Output "`nTotal unique missing tasks found: $missingCount"
