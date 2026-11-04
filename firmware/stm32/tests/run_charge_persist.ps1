param([string]$Gcc = 'gcc')
$ErrorActionPreference = 'Stop'
$workspace = Split-Path -Parent $PSScriptRoot
Push-Location -LiteralPath $workspace
try {
    New-Item -ItemType Directory -Force Output/Tests | Out-Null
    $common = @('-std=c99','-Wall','-Wextra','-Werror','-pedantic','-ICode','-Itests/stubs')
    & $Gcc @common Code/Battery_Settings.c Code/IP5310.c tests/test_battery_settings.c -o Output/Tests/settings.exe
    if ($LASTEXITCODE) { throw 'Journal build failed' }
    & ./Output/Tests/settings.exe
    if ($LASTEXITCODE) { throw 'Journal tests failed' }
    & $Gcc @common Code/Battery.c Code/Battery_Policy.c Code/IP5310.c Code/Battery_Settings.c tests/test_battery_persist.c -o Output/Tests/persist.exe
    if ($LASTEXITCODE) { throw 'Persistence build failed' }
    & ./Output/Tests/persist.exe
    if ($LASTEXITCODE) { throw 'Persistence tests failed' }
} finally { Pop-Location }
