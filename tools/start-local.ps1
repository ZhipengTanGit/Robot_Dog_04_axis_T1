param(
    [string]$NodePath = 'C:\Program Files\nodejs\node.exe'
)

$ErrorActionPreference = 'Stop'
$projectRoot = (Resolve-Path -LiteralPath (Join-Path $PSScriptRoot '..')).Path
$serverScript = Join-Path $PSScriptRoot 'serve.mjs'
$localUrl = 'http://127.0.0.1:8093/'
$port = 8093

function Get-PageState([int]$TimeoutSeconds = 3) {
    try {
        $response = Invoke-WebRequest -Uri $localUrl -UseBasicParsing `
            -TimeoutSec $TimeoutSeconds -MaximumRedirection 0 -ErrorAction Stop
        if ([int]$response.StatusCode -eq 200 -and
            $response.Content -match '(?is)<title[^>]*>[^<]*配色工作台[^<]*</title>') {
            return 'ready'
        }
        return 'occupied'
    } catch {
        if ($null -ne $_.Exception.Response) { return 'occupied' }
        return 'unavailable'
    }
}

function Test-LocalPort {
    $client = New-Object System.Net.Sockets.TcpClient
    $pending = $null
    try {
        $pending = $client.BeginConnect('127.0.0.1', $port, $null, $null)
        if (-not $pending.AsyncWaitHandle.WaitOne(300)) { return $false }
        $client.EndConnect($pending)
        return $client.Connected
    } catch {
        return $false
    } finally {
        if ($null -ne $pending) { $pending.AsyncWaitHandle.Close() }
        $client.Close()
    }
}

function Open-Workbench {
    Start-Process -FilePath $localUrl -ErrorAction Stop | Out-Null
    Write-Host "配色工作台已打开：$localUrl" -ForegroundColor Green
}

try {
    Write-Host '正在检查配色工作台…'
    $pageState = Get-PageState -TimeoutSeconds 3
    if ($pageState -eq 'ready') {
        Open-Workbench
        exit 0
    }
    if ($pageState -eq 'occupied' -or (Test-LocalPort)) {
        throw '本机 8093 端口正在被其他服务占用。请先确认并关闭占用它的程序，再双击启动。本启动器不会停止其他进程。'
    }
    if (-not (Test-Path -LiteralPath $NodePath -PathType Leaf)) {
        throw "没有找到 Node.js：$NodePath。请确认 Node.js 已安装；若安装位置不同，可修改 tools\start-local.ps1 开头的 NodePath。"
    }
    if (-not (Test-Path -LiteralPath $serverScript -PathType Leaf) -or
        -not (Test-Path -LiteralPath (Join-Path $projectRoot 'dist\index.html') -PathType Leaf)) {
        throw '项目文件不完整。请保留整个文件夹，并确认 tools\serve.mjs 与 dist\index.html 都在。'
    }

    $logDirectory = Join-Path $projectRoot 'review-output'
    New-Item -ItemType Directory -Path $logDirectory -Force | Out-Null
    $logStamp = (Get-Date -Format 'yyyyMMdd-HHmmss') + '-' + $PID
    $stdoutLog = Join-Path $logDirectory "local-$logStamp-stdout.log"
    $stderrLog = Join-Path $logDirectory "local-$logStamp-stderr.log"

    # Only this child process inherits PORT; no user/global environment is changed.
    $previousPort = $env:PORT
    try {
        $env:PORT = [string]$port
        $server = Start-Process -FilePath $NodePath -ArgumentList @("`"$serverScript`"") `
            -WorkingDirectory $projectRoot -WindowStyle Hidden -PassThru `
            -RedirectStandardOutput $stdoutLog -RedirectStandardError $stderrLog
    } finally {
        $env:PORT = $previousPort
    }

    Write-Host '正在启动本地服务…'
    $timer = [System.Diagnostics.Stopwatch]::StartNew()
    while ($timer.ElapsedMilliseconds -lt 10000) {
        $remaining = 10000 - $timer.ElapsedMilliseconds
        if ($remaining -lt 1000) { break }
        $timeout = [Math]::Min(3, [Math]::Floor($remaining / 1000))
        $pageState = Get-PageState -TimeoutSeconds ([int]$timeout)
        if ($pageState -eq 'ready') {
            Open-Workbench
            exit 0
        }
        if ($pageState -eq 'occupied') {
            throw '8093 端口返回了其他页面。为避免干扰其他服务，启动器不会继续操作或停止进程。'
        }
        $server.Refresh()
        if ($server.HasExited) { break }
        $remaining = 10000 - $timer.ElapsedMilliseconds
        if ($remaining -gt 0) { Start-Sleep -Milliseconds ([int][Math]::Min(200, $remaining)) }
    }
    throw "本地服务未能在 10 秒内就绪。请稍后重新双击；排查日志位于：$stderrLog"
} catch {
    Write-Host ''
    Write-Host ('启动未完成：' + $_.Exception.Message) -ForegroundColor Red
    exit 1
}
