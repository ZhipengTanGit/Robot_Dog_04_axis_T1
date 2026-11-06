param([string]$Gcc = 'gcc')
$ErrorActionPreference = 'Stop'
$workspace = Split-Path -Parent $PSScriptRoot
$testOutput = Join-Path $workspace 'Output\Tests'
New-Item -ItemType Directory -Force -Path $testOutput | Out-Null

function Invoke-Checked([string]$Program, [string[]]$Arguments) {
    & $Program @Arguments
    if ($LASTEXITCODE -ne 0) {
        throw "$Program failed with exit code $LASTEXITCODE"
    }
}

Push-Location -LiteralPath $workspace
try {
    $common = @('-std=c99', '-Wall', '-Wextra', '-Werror', '-pedantic', '-ICode', '-DBATTERY_CURRENT_PERSIST_ENABLED=0', '-DSERVO_TTL_ENABLED=0')
    $driverExe = Join-Path $testOutput 'test_ip5310.exe'
    Invoke-Checked $Gcc ($common + @('Code/IP5310.c', 'tests/test_ip5310.c', '-o', $driverExe))
    Invoke-Checked $driverExe @()

    $integration = @('-Itests/stubs', 'Code/Battery.c', 'Code/Battery_Policy.c',
        'Code/IP5310.c', 'tests/test_battery.c', '-lm')
    $monitorExe = Join-Path $testOutput 'test_battery_monitor.exe'
    Invoke-Checked $Gcc ($common + @('-DBATTERY_VBUS_CURRENT_SETUP_ENABLED=0', '-DBATTERY_KEEP_5V_ON_ENABLED=0') + $integration + @('-o', $monitorExe))
    Invoke-Checked $monitorExe @()

    $controlExe = Join-Path $testOutput 'test_battery_control.exe'
    $control = @('-DBATTERY_CONTROL_ENABLED=1', '-DBATTERY_REGISTER_MAP_CONFIRMED=1',
        '-DBATTERY_TEMP_ADC_CONNECTED=1', '-DBATTERY_VBUS_CURRENT_SETUP_ENABLED=0', '-DBATTERY_KEEP_5V_ON_ENABLED=0')
    Invoke-Checked $Gcc ($common + $control + $integration + @('-o', $controlExe))
    Invoke-Checked $controlExe @()
    $vbusExe = Join-Path $testOutput 'test_battery_vbus.exe'
    $vbus = @('-Itests/stubs', 'Code/Battery.c', 'Code/Battery_Policy.c',
        'Code/IP5310.c', 'tests/test_battery_vbus.c', '-o', $vbusExe)
    Invoke-Checked $Gcc ($common + $vbus)
    Invoke-Checked $vbusExe @()
    $powerExe = Join-Path $testOutput 'test_battery_power.exe'
    $power = @('-Itests/stubs', '-DBATTERY_VBUS_CURRENT_SETUP_ENABLED=0',
        'Code/Battery.c', 'Code/Battery_Policy.c', 'Code/IP5310.c',
        'tests/test_battery_power.c', '-o', $powerExe)
    Invoke-Checked $Gcc ($common + $power)
    Invoke-Checked $powerExe @()
    $bspExe = Join-Path $testOutput 'test_battery_bsp.exe'
    $bsp = @('-Itests/bsp_stubs', '-IBSP/inc', 'BSP/src/BSP_Battery.c',
        'tests/test_battery_bsp.c', '-o', $bspExe)
    Invoke-Checked $Gcc ($common + $bsp)
    Invoke-Checked $bspExe @()
    $ahtExe = Join-Path $testOutput 'test_aht20.exe'
    Invoke-Checked $Gcc ($common + @('-Itests/bsp_stubs', '-IBSP/inc', 'Code/AHT20.c', 'tests/test_aht20.c', '-o', $ahtExe))
    Invoke-Checked $ahtExe @()
    $periphExe = Join-Path $testOutput 'test_peripherals.exe'
    Invoke-Checked $Gcc ($common + @('-Dprintf=test_printf', '-Itests/bsp_stubs', '-IBSP/inc', 'Code/IP5310.c', 'Code/AHT20.c', 'Code/Peripherals.c', 'tests/test_peripherals.c', '-o', $periphExe))
    Invoke-Checked $periphExe @()
    $chargeLedExe = Join-Path $testOutput 'test_charge_led.exe'
    Invoke-Checked $Gcc ($common + @('-Dprintf=test_printf', '-Itests/bsp_stubs', '-IBSP/inc', 'Code/IP5310.c', 'Code/AHT20.c', 'Code/Peripherals.c', 'tests/test_charge_led.c', '-o', $chargeLedExe))
    Invoke-Checked $chargeLedExe @()
    $buzzerExe = Join-Path $testOutput 'test_buzzer_command.exe'
    Invoke-Checked $Gcc ($common + @('-Dprintf=test_printf', '-Itests/bsp_stubs', '-IBSP/inc', 'Code/IP5310.c', 'Code/AHT20.c', 'Code/Peripherals.c', 'tests/test_buzzer_command.c', '-o', $buzzerExe))
    Invoke-Checked $buzzerExe @()
    $periphBspExe = Join-Path $testOutput 'test_peripheral_bsp.exe'
    Invoke-Checked $Gcc ($common + @('-Itests/peripheral_bsp_stubs', '-IBSP/inc', 'BSP/src/BSP_Peripherals.c', 'tests/test_peripheral_bsp.c', '-o', $periphBspExe))
    Invoke-Checked $periphBspExe @()
    Write-Output 'All battery and peripheral host tests passed.'
} finally {
    Pop-Location
}
