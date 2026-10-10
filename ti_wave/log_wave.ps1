param(
    [string]$Port = '',
    [int]$Baud = 115200,
    [string]$Log = (Join-Path $PSScriptRoot ("wave_{0}.csv" -f (Get-Date -Format 'yyyyMMdd_HHmmss'))),
    [int]$DurationSeconds = 0
)
$ErrorActionPreference = 'Stop'
$Host.UI.RawUI.WindowTitle = 'TI Wave - live UART results (auto reconnect)'
$sw = [IO.StreamWriter]::new($Log, $true, [Text.UTF8Encoding]::new($true))
$sw.AutoFlush = $true
if ((Get-Item -LiteralPath $Log).Length -eq 0) { $sw.WriteLine('time,line') }
$started = Get-Date
$lastStatus = ''
$sp = $null
function Show-Status([string]$message) {
    if ($message -ne $script:lastStatus) {
        Write-Host $message -ForegroundColor Yellow
        $script:lastStatus = $message
    }
}
Write-Host "TI Wave UART monitor: $Baud 8N1; Ctrl+C to stop" -ForegroundColor Green
Write-Host "Log: $Log"
try {
    while (($DurationSeconds -eq 0) -or (((Get-Date) - $started).TotalSeconds -lt $DurationSeconds)) {
        try {
            if (!$sp -or !$sp.IsOpen) {
                $selected = $Port
                if (!$selected) {
                    $ports = @(Get-CimInstance Win32_PnPEntity -Filter "Name LIKE '%XDS110%'" |
                        Where-Object { $_.Status -eq 'OK' -and $_.Name -match 'Application/User UART \(COM\d+\)' })
                    if ($ports.Count -ne 1) {
                        Show-Status 'Waiting for one XDS110 Application/User UART. Connect the board USB; use -Port COMx if multiple boards.'
                        Start-Sleep -Seconds 1
                        continue
                    }
                    $selected = [regex]::Match($ports[0].Name, 'COM\d+').Value
                }
                $sp = [IO.Ports.SerialPort]::new($selected, $Baud, 'None', 8, 'One')
                $sp.Handshake = 'None'
                $sp.DtrEnable = $false
                $sp.RtsEnable = $false
                $sp.ReadTimeout = 500
                $sp.Open()
                Show-Status "Connected: $selected @ $Baud. Waiting for firmware data..."
                $pending = ''
                $lastData = Get-Date
                $warned = $false
            }
            $chunk = $sp.ReadExisting()
            if ($chunk.Length -gt 0) {
                $lastData = Get-Date
                $warned = $false
                $pending += $chunk
                while (($end = $pending.IndexOf("`n")) -ge 0) {
                    $line = $pending.Substring(0, $end).TrimEnd("`r")
                    $pending = $pending.Substring($end + 1)
                    if ($line.Length -eq 0) { continue }
                    $ts = (Get-Date).ToString('yyyy-MM-dd HH:mm:ss.fff')
                    Write-Host "[$ts] $line"
                    $sw.WriteLine(('"{0}","{1}"' -f $ts, $line.Replace('"', '""')))
                }
                if ($pending.Length -gt 4096) { Show-Status 'Received data without line endings; check baud rate.'; $pending = '' }
            }
            if (!$warned -and ((Get-Date) - $lastData).TotalSeconds -gt 5) {
                Show-Status 'No UART data for 5s. In CCS click Resume (F8), or reset the board; check TX/RX jumpers and firmware.'
                $warned = $true
            }
            Start-Sleep -Milliseconds 100
        } catch {
            Show-Status ("Serial unavailable: {0}. Retrying automatically..." -f $_.Exception.Message)
            if ($sp) { $sp.Dispose(); $sp = $null }
            Start-Sleep -Seconds 1
        }
    }
} finally {
    if ($sp) { $sp.Dispose() }
    $sw.Dispose()
}
