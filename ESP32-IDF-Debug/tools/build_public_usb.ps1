param(
    [ValidateSet('build', 'reconfigure')]
    [string]$Action = 'build',
    [string]$EspressifRoot = 'D:\05_Software_App\ESP32\Espressif'
)

$ErrorActionPreference = 'Stop'
$projectRoot = (Resolve-Path -LiteralPath (Join-Path $PSScriptRoot '..')).Path
$sdkRoot = Join-Path $EspressifRoot 'frameworks\esp-idf-v5.4.4'
$pythonRoot = Join-Path $EspressifRoot 'python_env\idf5.4_py3.11_env'
$python = Join-Path $pythonRoot 'Scripts\python.exe'
$idfPy = Join-Path $sdkRoot 'tools\idf.py'
if (-not (Test-Path -LiteralPath $python) -or -not (Test-Path -LiteralPath $idfPy)) {
    throw 'ESP-IDF 5.4.4 and its Python 3.11 environment are required.'
}

# Process-local SDK activation. Never call set-target or change the private
# sdkconfig/build_bridge cache. This script has no flash/erase/serial action.
$env:ComSpec = Join-Path $env:SystemRoot 'System32\cmd.exe'
$env:PATHEXT = '.COM;.EXE;.BAT;.CMD'
$env:PROCESSOR_ARCHITECTURE = 'AMD64'
$env:IDF_PATH = $sdkRoot
$env:IDF_TOOLS_PATH = $EspressifRoot
$env:IDF_PYTHON_ENV_PATH = $pythonRoot
$env:IDF_TARGET = 'esp32s3'
$env:PYTHONIOENCODING = 'utf-8'
$env:PATH = (Join-Path $pythonRoot 'Scripts') + ';' + $env:PATH
. (Join-Path $sdkRoot 'export.ps1')
if ($LASTEXITCODE -ne 0) { throw 'ESP-IDF environment activation failed.' }

& $python $idfPy -C $projectRoot -B (Join-Path $projectRoot 'build_public') `
    -D "SDKCONFIG=$projectRoot/sdkconfig.public" `
    -D "SDKCONFIG_DEFAULTS=$projectRoot/sdkconfig.defaults" `
    -D 'IDF_TARGET=esp32s3' -D 'ROBOT_PUBLIC_TEST_BUILD=ON' $Action
if ($LASTEXITCODE -ne 0) { throw "Public USB build failed: $LASTEXITCODE" }
