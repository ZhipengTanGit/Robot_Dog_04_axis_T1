param([string]$KeilPath = $env:KEIL_UV4)
$ErrorActionPreference = 'Stop'
$workspace = Split-Path -Parent $PSScriptRoot
if (!$KeilPath -or !(Test-Path -LiteralPath $KeilPath)) {
    throw 'Pass -KeilPath with your installed UV4.exe path, or set KEIL_UV4.'
}
New-Item -ItemType Directory -Force -Path (Join-Path $workspace 'Output') | Out-Null
$project = Join-Path $workspace 'Project\main.uvprojx'
$buildLog = Join-Path $workspace 'Output\build.log'
$process = Start-Process -FilePath $KeilPath -ArgumentList @('-r',('"'+$project+'"'),'-t','"Target 1"','-o',('"'+$buildLog+'"')) -WindowStyle Hidden -Wait -PassThru
$log = [IO.File]::ReadAllText($buildLog)
if ($process.ExitCode -gt 1 -or $log -notmatch '0 Error\(s\), 0 Warning\(s\)') { throw "Build failed; inspect $buildLog" }
$main = [IO.File]::ReadAllText((Join-Path $workspace 'Code\main.c'))
$match = [regex]::Match($main, 'fw=ACTION_TEACH_(stm32f103rb-action-teach-[a-z0-9-]+)')
if (!$match.Success) { throw 'Firmware version missing.' }
& python -X utf8 (Join-Path $PSScriptRoot 'package_action_teach.py') --workspace $workspace --output (Join-Path $workspace 'Output\firmware') --version $match.Groups[1].Value
if ($LASTEXITCODE) { throw 'Firmware package validation failed.' }
Write-Output 'Build and package complete. No device was flashed.'
