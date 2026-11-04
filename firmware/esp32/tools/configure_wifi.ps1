param(
    [Parameter(Mandatory=$true)][string]$Ssid,
    [Parameter(Mandatory=$true)][string]$WindowsProfile,
    [Parameter(Mandatory=$true)][string]$TargetIp,
    [ValidateRange(1,65535)][int]$TargetPort = 9000,
    [ValidateRange(1,65535)][int]$LocalPort = 9001
)
$ErrorActionPreference = 'Stop'
$projectRoot = Split-Path -Parent $PSScriptRoot
$address = $null
if (-not [System.Net.IPAddress]::TryParse($TargetIp, [ref]$address) -or
    $address.AddressFamily -ne [System.Net.Sockets.AddressFamily]::InterNetwork) {
    throw 'TargetIp must be an IPv4 address.'
}
if ([Text.Encoding]::UTF8.GetByteCount($Ssid) -gt 32 -or $Ssid.Length -eq 0) {
    throw 'SSID must contain 1 to 32 UTF-8 bytes.'
}
# Capture only the explicitly selected profile. Never print its clear-text key.
$profileText = (& netsh wlan show profile "name=$WindowsProfile" key=clear) -join "`n"
if ($LASTEXITCODE -ne 0) { throw 'Could not read the selected Windows Wi-Fi profile.' }
$match = [regex]::Match($profileText, '(?m)^\s*(?:Key Content|关键内容|金鑰內容)\s*:\s*(.+?)\r?$')
if (-not $match.Success) { throw 'The selected profile has no readable personal Wi-Fi key.' }
$networkKey = $match.Groups[1].Value
$keyLength = [Text.Encoding]::UTF8.GetByteCount($networkKey)
if ($keyLength -lt 8 -or $keyLength -gt 64) { throw 'Unsupported Wi-Fi key length.' }
function ConvertTo-CByteString([string]$Value) {
    # Fixed-width octal escapes preserve UTF-8 and do not expose shell syntax.
    $encoded = foreach ($valueByte in [Text.Encoding]::UTF8.GetBytes($Value)) {
        '\' + [Convert]::ToString($valueByte, 8).PadLeft(3, '0')
    }
    return '"' + ($encoded -join '') + '"'
}
$header = @(
    '#ifndef NETWORK_CONFIG_LOCAL_H',
    '#define NETWORK_CONFIG_LOCAL_H',
    '/* Local provisioning file. Keep out of source control and shared builds. */',
    ('#define ROBOT_WIFI_SSID ' + (ConvertTo-CByteString $Ssid)),
    ('#define ROBOT_WIFI_PASSWORD ' + (ConvertTo-CByteString $networkKey)),
    ('#define ROBOT_UDP_TARGET_IP "' + $address.ToString() + '"'),
    ('#define ROBOT_UDP_TARGET_PORT ' + $TargetPort),
    ('#define ROBOT_UDP_LOCAL_PORT ' + $LocalPort),
    '#endif',
    ''
) -join "`n"
$outputPath = Join-Path $projectRoot 'Bsp\NetworkConfig.local.h'
[IO.File]::WriteAllText($outputPath, $header, (New-Object Text.UTF8Encoding($false)))
$networkKey = $null
$profileText = $null
$header = $null
Write-Output "Configured SSID '$Ssid', UDP destination ${TargetIp}:$TargetPort, ESP local port $LocalPort. Password not displayed."
