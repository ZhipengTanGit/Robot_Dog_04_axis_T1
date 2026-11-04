param(
    [string]$KeilPath = 'D:\05_Software_App\Keil_v536\Keil_v5\UV4\UV4.exe',
    [string]$Gcc = 'gcc',
    [string]$Python = 'py',
    [string]$ReleaseName = 'stm32f103rb-public-test-350ma-20261029-r2'
)
$ErrorActionPreference = 'Stop'
$workspace = Split-Path -Parent $PSScriptRoot
if ($ReleaseName -notmatch '^stm32f103rb-public-test-350ma-[a-z0-9-]+$') { throw 'Invalid public-test release name' }
if (-not (Test-Path -LiteralPath $KeilPath)) { throw 'Keil uVision was not found' }
$release = Join-Path $workspace ('Output\public-tests\' + $ReleaseName)
if (Test-Path -LiteralPath $release) { throw 'Release already exists; choose a new -ReleaseName. No build files were overwritten.' }
$snapshot = Join-Path $release 'workspace'
$public = Join-Path $release 'public'
New-Item -ItemType Directory -Path $snapshot,$public | Out-Null
foreach ($folder in @('Code','BSP','Core','Drivers','Startup','tests')) {
    Copy-Item -LiteralPath (Join-Path $workspace $folder) -Destination (Join-Path $snapshot $folder) -Recurse
}
New-Item -ItemType Directory -Path (Join-Path $snapshot 'Project') | Out-Null
Copy-Item -LiteralPath (Join-Path $workspace 'Project\main.uvprojx') -Destination (Join-Path $snapshot 'Project\main.uvprojx')
$configPath = Join-Path $snapshot 'Code\Battery_Config.h'
$config = [System.IO.File]::ReadAllText($configPath)
foreach($name in @('BATTERY_PROJECT_NORMAL_LIMIT_MA','BATTERY_VBUS_REQUESTED_MA','BATTERY_VBUS_NOMINAL_MA','BATTERY_BOARD_MAX_NOMINAL_MA')) {
    $pattern = '(?m)^#define\s+' + $name + '\s+\d+U\s*$'
    if ([regex]::Matches($config,$pattern).Count -ne 1) { throw "Expected one profile definition: $name" }
    $config = [regex]::Replace($config,$pattern,('#define ' + $name + ' 350U'))
}
$config = [regex]::Replace($config,'(?m)^#define BATTERY_CONTROL_ENABLED\s+\d+\s*$','#define BATTERY_CONTROL_ENABLED 0')
$config = [regex]::Replace($config,'(?m)^#define BATTERY_KEEP_5V_ON_ENABLED\s+\d+\s*$','#define BATTERY_KEEP_5V_ON_ENABLED 1')
[System.IO.File]::WriteAllText($configPath,$config,[System.Text.UTF8Encoding]::new($false))
$mainPath = Join-Path $snapshot 'Code\main.c'
$main = [System.IO.File]::ReadAllText($mainPath)
if (-not $main.Contains('fw=BUS_RECOVERY_1')) { throw 'Expected firmware banner was not found' }
$main = $main.Replace('fw=BUS_RECOVERY_1',('fw=PUBLIC_TEST_350MA_' + $ReleaseName))
[System.IO.File]::WriteAllText($mainPath,$main,[System.Text.UTF8Encoding]::new($false))
# The legacy integration fixture used 650/850mA as accepted examples. Retain
# every assertion while substituting valid public-profile 250/350mA examples.
$fixturePath = Join-Path $snapshot 'tests\test_peripherals.c'
$fixture = [System.IO.File]::ReadAllText($fixturePath)
$fixtureStart = $fixture.IndexOf('static void test_current_command_protocol(void)')
if ($fixtureStart -lt 0) { throw 'Expected current-command integration fixture was not found' }
$fixtureTail = $fixture.Substring($fixtureStart)
$fixtureTail = [regex]::Replace($fixtureTail,'(?<!\d)1850(?!\d)','350')
$fixtureTail = [regex]::Replace($fixtureTail,'(?<!\d)650(?!\d)','250')
$fixtureTail = [regex]::Replace($fixtureTail,'(?<!\d)850(?!\d)','350')
[System.IO.File]::WriteAllText($fixturePath,($fixture.Substring(0,$fixtureStart)+$fixtureTail),[System.Text.UTF8Encoding]::new($false))
$privateLog = Join-Path $release 'build-private.log'
$projectFile = Join-Path $snapshot 'Project\main.uvprojx'
$process = Start-Process -FilePath $KeilPath -ArgumentList @('-r',('"'+$projectFile+'"'),'-t','"Target 1"','-o',('"'+$privateLog+'"')) -WindowStyle Hidden -Wait -PassThru
$logText = [System.IO.File]::ReadAllText($privateLog)
if ($process.ExitCode -gt 1 -or $logText -notmatch '0 Error\(s\), 0 Warning\(s\)') { throw 'Isolated public-test compile failed; inspect the private log locally.' }
$testPath = Join-Path $snapshot 'tests\test_public_profile.c'
$testSource = @'
#define main private_legacy_vbus_main
#include "test_battery_vbus.c"
#undef main
int main(void) {
    unsigned i;
    uint8_t code;
    uint16_t nominal;
    Battery_Status s;
    assert(BATTERY_VBUS_NOMINAL_MA == 350U && BATTERY_VBUS_REQUESTED_MA == 350U);
    assert(BATTERY_BOARD_MAX_NOMINAL_MA == 350U && BATTERY_KEEP_5V_ON_ENABLED == 1);
    assert(BATTERY_CONTROL_ENABLED == 0 && BATTERY_TEMP_ADC_CONNECTED == 0);
    assert(IP5310_VbusCodeFromNominal(350U, &code) == IP5310_OK && code == 3U);
    assert(IP5310_VbusCodeFromNominal(500U, &code) != IP5310_OK);
    reset(0xf8U, 0xadU); regs[0x02] = 0x65U; step();
    s = status();
    assert(s.link == BATTERY_LINK_READY && s.control_verified && s.applied_nominal_ma == 350U);
    assert(regs[0x26] == 0xa3U && regs[0x00] == 0xf8U && enable_writes == 0U);
    assert((regs[0x02] & 0xe0U) == 0xe0U && s.keep_5v_verified);
    for (i=0U;i<32U;++i) {
        nominal = IP5310_VbusNominalFromCode((uint8_t)i);
        if (nominal>350U) assert(Battery_RequestNominalCurrent(nominal)==BATTERY_CURRENT_BOARD_LIMIT);
    }
    assert(Battery_RequestNominalCurrent(500U)==BATTERY_CURRENT_UNSUPPORTED);
    assert(Battery_RequestNominalCurrent(250U)==BATTERY_CURRENT_ACCEPTED); step();
    assert(status().applied_nominal_ma==250U && enable_writes==0U);
    reset(0xe8U,0xadU); step();
    assert(status().applied_nominal_ma==350U && regs[0x00]==0xe8U && enable_writes==0U);
    puts("Public test profile passed: default and cap 350mA, keep5V, no charge-enable writes.");
    return 0;
}
'@
# The public profile adds assertions without changing the original test sources.
[System.IO.File]::WriteAllText($testPath,$testSource,[System.Text.UTF8Encoding]::new($false))
$testOutput = Join-Path $release 'host-tests-private.log'
$testLines = [System.Collections.Generic.List[string]]::new()
function Invoke-Checked([string]$Program,[string[]]$Arguments) {
    $lines = & $Program @Arguments 2>&1
    if ($LASTEXITCODE -ne 0) { $lines | Out-String | Write-Error; throw 'Public-test host validation failed' }
    foreach($line in $lines) { $testLines.Add([string]$line) }
}
Push-Location -LiteralPath $snapshot
try {
    New-Item -ItemType Directory -Force -Path 'Output\Tests' | Out-Null
    $common = @('-std=c99','-Wall','-Wextra','-Werror','-pedantic','-ICode')
    Invoke-Checked $Gcc ($common + @('-Itests/stubs','Code/Battery.c','Code/Battery_Policy.c','Code/IP5310.c','tests/test_public_profile.c','-o','Output/Tests/public-profile.exe'))
    Invoke-Checked '.\Output\Tests\public-profile.exe' @()
    Invoke-Checked $Gcc ($common + @('Code/IP5310.c','tests/test_ip5310.c','-o','Output/Tests/ip5310.exe'))
    Invoke-Checked '.\Output\Tests\ip5310.exe' @()
    Invoke-Checked $Gcc ($common + @('-Dprintf=test_printf','-Itests/bsp_stubs','-IBSP/inc','Code/IP5310.c','Code/AHT20.c','Code/Peripherals.c','tests/test_buzzer_command.c','-o','Output/Tests/buzzer.exe'))
    Invoke-Checked '.\Output\Tests\buzzer.exe' @()
    Invoke-Checked $Gcc ($common + @('-Itests/peripheral_bsp_stubs','-IBSP/inc','BSP/src/BSP_Peripherals.c','tests/test_peripheral_bsp.c','-o','Output/Tests/peripheral-bsp.exe'))
    Invoke-Checked '.\Output\Tests\peripheral-bsp.exe' @()
} finally { Pop-Location }
[System.IO.File]::WriteAllLines($testOutput,$testLines,[System.Text.UTF8Encoding]::new($false))
& $Python -3.14 (Join-Path $PSScriptRoot 'package_public_test.py') --workspace $snapshot --public $public --version $ReleaseName
if ($LASTEXITCODE -ne 0) { throw 'Public-test packaging failed' }
Write-Output ('Public release (only this directory may be published): ' + $public)
