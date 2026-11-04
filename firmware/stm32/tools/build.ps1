param(
    [string]$KeilPath = 'D:\05_Software_App\Keil_v536\Keil_v5\UV4\UV4.exe'
)
$ErrorActionPreference = 'Stop'
$workspace = Split-Path -Parent $PSScriptRoot
if (-not (Test-Path -LiteralPath $KeilPath)) {
    throw 'Keil uVision not found. Pass -KeilPath with the installed UV4.exe path.'
}
$projectFile = Join-Path $workspace 'Project\main.uvprojx'
$logFile = Join-Path $workspace 'Output\battery-build.log'
New-Item -ItemType Directory -Force -Path (Split-Path -Parent $logFile) | Out-Null
$process = Start-Process -FilePath $KeilPath -ArgumentList @(
    '-r', ('"' + $projectFile + '"'), '-t', '"Target 1"', '-o', ('"' + $logFile + '"')
) -WindowStyle Hidden -Wait -PassThru
if (-not (Test-Path -LiteralPath $logFile)) {
    throw "Keil did not produce a build log. Exit code: $($process.ExitCode)"
}
$logText = Get-Content -LiteralPath $logFile -Raw
Write-Output $logText
if ($process.ExitCode -gt 1 -or $logText -notmatch '0 Error\(s\)') {
    throw "Keil build failed. See $logFile"
}
Write-Output ('HEX: ' + (Join-Path $workspace 'Output\Objects\main.hex'))
