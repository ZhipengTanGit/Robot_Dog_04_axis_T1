$ErrorActionPreference = 'Stop'
$projectTaskRoot = Split-Path -Parent $PSScriptRoot
$pythonTask = 'D:\05_Software_App\ESP32\Espressif\python_env\idf5.5_py3.11_env\Scripts\pythonw.exe'
if (-not (Test-Path -LiteralPath $pythonTask)) { throw 'Python runtime was not found.' }
$launcherTask = Join-Path $PSScriptRoot 'launch_windows.py'
$desktopTask = [Environment]::GetFolderPath('Desktop')
$shellTask = New-Object -ComObject WScript.Shell
$shortcutTasks = @(
    @{Name='Robot Monitor - USB'; Args='--mode serial --gui'; Icon='shell32.dll,17'},
    @{Name='Robot Monitor - Stop'; Args='--stop-all --gui'; Icon='shell32.dll,27'}
)
foreach ($itemTask in $shortcutTasks) {
    $targetTask = Join-Path $desktopTask ($itemTask.Name + '.lnk')
    if (Test-Path -LiteralPath $targetTask) {
        $oldTask = $shellTask.CreateShortcut($targetTask)
        if ($oldTask.Arguments -notlike ('*' + $launcherTask + '*')) { throw "Existing unrelated shortcut: $targetTask" }
    }
    $shortcutTask = $shellTask.CreateShortcut($targetTask)
    $shortcutTask.TargetPath = $pythonTask
    $shortcutTask.Arguments = '"' + $launcherTask + '" ' + $itemTask.Args
    $shortcutTask.WorkingDirectory = $projectTaskRoot
    $shortcutTask.IconLocation = $itemTask.Icon
    $shortcutTask.Description = $itemTask.Name
    $shortcutTask.Save()
    Write-Output $targetTask
}
