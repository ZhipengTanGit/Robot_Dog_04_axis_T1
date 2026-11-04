param([string]$Gcc = 'gcc')
$ErrorActionPreference = 'Stop'
$projectRoot = Split-Path -Parent $PSScriptRoot
$outputDir = Join-Path $projectRoot 'review-output\audio-recovery'
New-Item -ItemType Directory -Force -Path $outputDir | Out-Null
$logFile = Join-Path $outputDir 'results.log'
$testLines = [System.Collections.Generic.List[string]]::new()
function Invoke-Checked([string]$Program, [string[]]$Arguments) {
    $lines = & $Program @Arguments 2>&1
    foreach ($line in $lines) { $testLines.Add([string]$line); Write-Output $line }
    if ($LASTEXITCODE -ne 0) { throw "$Program failed with exit code $LASTEXITCODE" }
}
Push-Location -LiteralPath $projectRoot
try {
    $common = @('-std=c11','-Wall','-Wextra','-Werror','-Itests/audio_driver_stubs','-IBsp')
    Invoke-Checked $Gcc ($common + @('tests/test_audio_recovery.c','Bsp/PeripheralMath.c','-lm','-o',"$outputDir/test_audio_recovery.exe"))
    Invoke-Checked "$outputDir/test_audio_recovery.exe" @()
    Invoke-Checked $Gcc ($common + @('tests/test_audio_tone_control.c','-o',"$outputDir/test_audio_tone_control.exe"))
    Invoke-Checked "$outputDir/test_audio_tone_control.exe" @()
    Invoke-Checked $Gcc ($common + @('-DAUDIO_PCM_HOST_TEST','-DAUDIO_MONITOR_HOST_TEST','Bsp/Bsp_Audio.c','Bsp/Bsp_AudioMonitor.c','tests/test_audio_monitor.c','-o',"$outputDir/test_audio_monitor.exe"))
    Invoke-Checked "$outputDir/test_audio_monitor.exe" @()
} finally {
    Pop-Location
    [System.IO.File]::WriteAllLines($logFile,$testLines,[System.Text.UTF8Encoding]::new($false))
}
