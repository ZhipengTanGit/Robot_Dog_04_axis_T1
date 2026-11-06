# One explicit inbound rule for the local robot receiver. No firewall disabling.
$ErrorActionPreference = 'Stop'
$ruleTask = 'RobotMonitor-LAN-8080'
$identityTask = [Security.Principal.WindowsIdentity]::GetCurrent()
if (-not ([Security.Principal.WindowsPrincipal]$identityTask).IsInRole([Security.Principal.WindowsBuiltInRole]::Administrator)) {
    $argumentsTask = '-NoProfile -ExecutionPolicy Bypass -File "' + $PSCommandPath + '"'
    Start-Process -FilePath 'powershell.exe' -ArgumentList $argumentsTask -Verb RunAs -WindowStyle Hidden -Wait
    exit
}
$existingTask = Get-NetFirewallRule -Name $ruleTask -ErrorAction SilentlyContinue
if ($existingTask) {
    Set-NetFirewallRule -Name $ruleTask -Enabled True -Direction Inbound -Action Allow -Profile Private -RemoteAddress LocalSubnet -Protocol TCP -LocalPort 8080 | Out-Null
} else {
    New-NetFirewallRule -Name $ruleTask -DisplayName 'Robot Monitor LAN TCP 8080' -Description 'Robot telemetry receiver: private LAN only, TCP 8080.' -Direction Inbound -Action Allow -Enabled True -Profile Private -Protocol TCP -LocalPort 8080 -RemoteAddress LocalSubnet | Out-Null
}
