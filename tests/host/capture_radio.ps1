param(
    [string]$Port = 'COM9',
    [int]$Seconds = 90,
    [string]$OutputDirectory = 'build/fault_validation'
)
$ErrorActionPreference = 'Stop'

$available = [System.IO.Ports.SerialPort]::GetPortNames()
if ($available -notcontains $Port) {
    $list = if ($available) { $available -join ', ' } else { 'none (is the radio plugged in?)' }
    throw "Port $Port not found. Available: $list - rerun with -Port <name>."
}

$captureFile = Join-Path $OutputDirectory 'radio_chunks.jsonl'
if (Test-Path $captureFile) { throw "$captureFile already exists - use a new -OutputDirectory." }

New-Item -ItemType Directory -Force -Path $OutputDirectory | Out-Null
$serial = New-Object System.IO.Ports.SerialPort($Port,115200,[System.IO.Ports.Parity]::None,8,[System.IO.Ports.StopBits]::One)
$serial.ReadTimeout = 100
$serial.ReadBufferSize = 65536
$serial.RtsEnable = $true
$serial.DtrEnable = $true
$writer = $null
try {
    try { $serial.Open() }
    catch [System.UnauthorizedAccessException] {
        throw "Port $Port is in use by another program (usually the ground station). Stop it, or capture while it is off."
    }
    $writer = New-Object System.IO.StreamWriter((Join-Path (Resolve-Path $OutputDirectory).Path 'radio_chunks.jsonl'),$false)
    $writer.AutoFlush = $true
    $timer = [System.Diagnostics.Stopwatch]::StartNew()
    $buffer = New-Object byte[] 4096
    $bytes = 0
    Write-Output "Radio capture started: $Port, $Seconds seconds, receive only"
    while ($timer.Elapsed.TotalSeconds -lt $Seconds) {
        try { $count = $serial.Read($buffer,0,$buffer.Length) } catch [System.TimeoutException] { continue }
        if ($count -gt 0) {
            $bytes += $count
            $record = @{utc=[DateTimeOffset]::UtcNow.ToUnixTimeMilliseconds(); elapsedMs=$timer.ElapsedMilliseconds; data=[Convert]::ToBase64String($buffer,0,$count)}
            $writer.WriteLine(($record | ConvertTo-Json -Compress))
        }
    }
    Write-Output "Radio capture complete: $bytes bytes"
} finally {
    if ($writer) { $writer.Dispose() }
    if ($serial.IsOpen) { $serial.Close() }
    $serial.Dispose()
}
