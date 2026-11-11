param(
    [int]$WaitSeconds = 240,
    [string]$Out = "E:\workspace\Repository\UniXNU-tb365fc-work\reader\log.bin"
)
$ErrorActionPreference = "Stop"

function Find-ReaderPort {
    try {
        $ports = Get-CimInstance Win32_SerialPort -ErrorAction SilentlyContinue
    } catch { return $null }
    foreach ($p in $ports) {
        if ($p.PNPDeviceID -match 'VID_1209.*PID_0001') { return $p.DeviceID }
    }
    return $null
}

$port = $null
$deadline = (Get-Date).AddSeconds($WaitSeconds)
while ((Get-Date) -lt $deadline) {
    $port = Find-ReaderPort
    if ($port) { break }
    Start-Sleep -Milliseconds 1000
}
if (-not $port) { throw "gadget 18d1:4ee7 did not appear within $WaitSeconds s" }
Write-Host "reader port: $port"

$sp = New-Object System.IO.Ports.SerialPort $port, 115200, 'None', 8, 'One'
$sp.ReadTimeout = 1000
$sp.WriteTimeout = 1000
$sp.DtrEnable = $true
$sp.RtsEnable = $true
$sp.Open()
Write-Host "opened $port"

$buf = New-Object System.Text.StringBuilder
$deadline = (Get-Date).AddSeconds(420)
$payload = $null
$win = 0
$crc = ''
while ((Get-Date) -lt $deadline) {
    try {
        $chunk = $sp.ReadExisting()
    } catch { $chunk = "" }
    if ($chunk) { [void]$buf.Append($chunk) }
    else { Start-Sleep -Milliseconds 100 }
    $text = $buf.ToString()
    $idx = $text.IndexOf("`nEND`n")
    if ($idx -ge 0) { $payload = $text.Substring(0, $idx); break }
}
if ($null -eq $payload) { throw "stream did not end ($($buf.Length) chars)" }

foreach ($line in ($payload -split "`r?`n")) {
    if ($line -like "TBLOG1*") { Write-Host "dev> $line" }
    if ($line -match "WIN\s+(\d+)\s+crc=([0-9a-fA-F]+)") {
        $win = [int]$matches[1]; $crc = $matches[2]
        Write-Host "dev> $line"
    }
    if ($line -notlike "TBLOG1*" -and $line -notmatch "^WIN " -and $line.Length -gt 200) { break }
}
if (-not $win) { throw "no WIN header in stream" }
$b64 = ($payload -replace "`r?`n", "")
$b64 = [regex]::Replace($b64, "TBLOG1[^A-Za-z0-9+/=]*", "")
$b64 = [regex]::Replace($b64, "WIN\s+\d+\s+crc=[0-9a-fA-F]+", "")
$b64 = [regex]::Replace($b64, "[^A-Za-z0-9+/=]", "")
$bytes = [Convert]::FromBase64String($b64)
if ($bytes.Length -lt $win) { throw "decoded $($bytes.Length) bytes, expected $win" }
[System.IO.File]::WriteAllBytes($Out, $bytes[0..($win - 1)])
Write-Host "wrote $Out ($win bytes, header crc $crc)"
$sp.Close()
