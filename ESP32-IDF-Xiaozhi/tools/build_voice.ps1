param(
    [ValidateSet('build', 'reconfigure', 'menuconfig')]
    [string]$Action = 'build',
    [string]$IdfPath = 'D:\05_Software_App\ESP32\Espressif\frameworks\esp-idf-v5.4.4'
)

$ErrorActionPreference = 'Stop'
$projectRoot = (Resolve-Path -LiteralPath (Join-Path $PSScriptRoot '..')).Path
$sdkRoot = (Resolve-Path -LiteralPath $IdfPath).Path
$toolsRoot = Split-Path -Parent (Split-Path -Parent $sdkRoot)
$pythonRoot = Join-Path $toolsRoot 'python_env\idf5.4_py3.11_env'
$python = Join-Path $pythonRoot 'Scripts\python.exe'
$idfPy = Join-Path $sdkRoot 'tools\idf.py'
$versionFile = Join-Path $sdkRoot 'tools\cmake\version.cmake'

if (-not (Test-Path -LiteralPath $python) -or -not (Test-Path -LiteralPath $idfPy)) {
    throw 'ESP-IDF 5.4.4 or its isolated Python 3.11 environment is missing.'
}
$versionText = Get-Content -Raw -LiteralPath $versionFile
if ($versionText -notmatch 'set\(IDF_VERSION_MAJOR 5\)' -or
    $versionText -notmatch 'set\(IDF_VERSION_MINOR 4\)' -or
    $versionText -notmatch 'set\(IDF_VERSION_PATCH 4\)') {
    throw 'This build script requires the validated ESP-IDF 5.4.4 SDK.'
}

# These variables affect this PowerShell process only; other SDKs stay installed.
$env:ComSpec = Join-Path $env:SystemRoot 'System32\cmd.exe'
$env:PATHEXT = '.COM;.EXE;.BAT;.CMD'
$env:PROCESSOR_ARCHITECTURE = 'AMD64'
$env:IDF_PATH = $sdkRoot
$env:IDF_TOOLS_PATH = $toolsRoot
$env:IDF_PYTHON_ENV_PATH = $pythonRoot
$env:IDF_TARGET = 'esp32s3'
$env:PYTHONIOENCODING = 'utf-8'
$env:PATH = (Join-Path $pythonRoot 'Scripts') + ';' + $env:PATH
$gitBin = Join-Path $env:USERPROFILE 'scoop\apps\git\current\bin'
if (Test-Path -LiteralPath (Join-Path $gitBin 'git.exe')) {
    $env:PATH = $gitBin + ';' + $env:PATH
}

# The local proxy interrupts TLS for Espressif's component downloads.
# Keep certificate verification enabled and bypass that proxy for these hosts.
$directHosts = @('components-file.espressif.com', 'components.espressif.com', 'dl.espressif.com')
$env:NO_PROXY = ((@($env:NO_PROXY -split ',') + $directHosts) |
    Where-Object { $_ } | Select-Object -Unique) -join ','

. (Join-Path $sdkRoot 'export.ps1')
if ($LASTEXITCODE -ne 0) {
    throw 'ESP-IDF environment activation failed.'
}
$preferredCompiler = Join-Path $toolsRoot 'tools\xtensa-esp-elf\esp-14.2.0_20260121\xtensa-esp-elf\bin'
if (Test-Path -LiteralPath (Join-Path $preferredCompiler 'xtensa-esp32s3-elf-gcc.exe')) {
    $env:PATH = $preferredCompiler + ';' + $env:PATH
}

$defaults = 'sdkconfig.defaults;sdkconfig.defaults.esp32s3;sdkconfig.robot'
& $python $idfPy -C $projectRoot -B (Join-Path $projectRoot 'build_robot') `
    -D "SDKCONFIG_DEFAULTS=$defaults" -D 'IDF_TARGET=esp32s3' $Action
exit $LASTEXITCODE
