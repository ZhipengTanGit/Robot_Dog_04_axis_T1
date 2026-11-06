param([string]$Gcc = 'gcc')
$ErrorActionPreference = 'Stop'
$workspace = Split-Path -Parent $PSScriptRoot
Push-Location -LiteralPath $workspace
try {
    New-Item -ItemType Directory -Force Output/Tests | Out-Null
    $common = @('-std=c99','-Wall','-Wextra','-Werror','-pedantic','-ICode')
    & $Gcc @common Code/TTLServo.c Code/TTLServo_Motion.c Code/TTLServo_Protocol.c tests/test_ttl_servo.c -o Output/Tests/ttl.exe
    if ($LASTEXITCODE) { throw 'TTL protocol build failed' }
    & ./Output/Tests/ttl.exe
    if ($LASTEXITCODE) { throw 'TTL protocol tests failed' }
    & $Gcc @common Code/TTLServo_Motion.c Code/TTLServo_Protocol.c tests/test_ttl_motion.c -o Output/Tests/ttl-motion.exe
    if ($LASTEXITCODE) { throw 'TTL motion build failed' }
    & ./Output/Tests/ttl-motion.exe
    if ($LASTEXITCODE) { throw 'TTL motion tests failed' }
    & $Gcc @common -Dprintf=test_printf -Itests/bsp_stubs -IBSP/inc Code/IP5310.c Code/AHT20.c Code/Peripherals.c Code/TTLServo_Motion.c Code/TTLServo_Protocol.c tests/test_teaching_commands.c -o Output/Tests/ttl-teaching.exe
    if ($LASTEXITCODE) { throw 'TTL teaching command build failed' }
    & ./Output/Tests/ttl-teaching.exe
    if ($LASTEXITCODE) { throw 'TTL teaching commands failed' }
    & $Gcc @common -Wno-pointer-to-int-cast -Itests/ttl_bsp_stubs Code/TTLServo_Protocol.c tests/test_ttl_bsp.c -o Output/Tests/ttl-bsp.exe
    if ($LASTEXITCODE) { throw 'TTL DMA build failed' }
    & ./Output/Tests/ttl-bsp.exe
    if ($LASTEXITCODE) { throw 'TTL DMA tests failed' }
    & $Gcc @common -Itests/peripheral_bsp_stubs -IBSP/inc BSP/src/BSP_Peripherals.c tests/test_peripheral_bsp.c -o Output/Tests/ttl-no-pwm.exe
    if ($LASTEXITCODE) { throw 'TTL PWM exclusion build failed' }
    & ./Output/Tests/ttl-no-pwm.exe
    if ($LASTEXITCODE) { throw 'TTL PWM exclusion tests failed' }
} finally { Pop-Location }
