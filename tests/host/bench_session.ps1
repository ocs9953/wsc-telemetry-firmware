param(
    [switch]$Flash,
    [switch]$StopOpenOcd,
    [switch]$NoCan,
    [string]$BuildDir = 'build/bench',
    [string]$ProbeSerial = '2'
)
$ErrorActionPreference = 'Stop'
$root = (Resolve-Path (Join-Path $PSScriptRoot '../..')).Path
Set-Location $root

$cubeIde = 'C:\ST\STM32CubeIDE_2.2.0\STM32CubeIDE\plugins'
$gcc = Get-ChildItem -Path $cubeIde -Filter 'arm-none-eabi-gcc.exe' -Recurse -ErrorAction SilentlyContinue |
    Select-Object -First 1
$makeExe = Get-ChildItem -Path $cubeIde -Filter 'make.exe' -Recurse -ErrorAction SilentlyContinue |
    Select-Object -First 1
$openocd = Get-ChildItem -Path $cubeIde -Filter 'openocd.exe' -Recurse -ErrorAction SilentlyContinue |
    Select-Object -First 1
if (-not $gcc -or -not $makeExe -or -not $openocd) { throw 'Toolchain not found under ' + $cubeIde }
$toolBin = Split-Path $gcc.FullName
$env:PATH = "$toolBin;$(Split-Path $makeExe.FullName);$env:PATH"
$targetCfg = Get-ChildItem -Path $cubeIde -Filter 'stm32u5x.cfg' -Recurse -ErrorAction SilentlyContinue |
    Select-Object -First 1
if (-not $targetCfg) { throw 'OpenOCD target scripts not found under ' + $cubeIde }
$scripts = Split-Path (Split-Path $targetCfg.FullName)

function Write-Step($text) { Write-Output "`n== $text" }

function Test-Port4444 {
    $c = New-Object System.Net.Sockets.TcpClient
    try {
        $task = $c.ConnectAsync('127.0.0.1', 4444)
        return ($task.Wait(300) -and $c.Connected)
    } catch { return $false }
    finally { $c.Close() }
}

if ($StopOpenOcd) {
    $running = Get-Process openocd -ErrorAction SilentlyContinue
    $running | Stop-Process -Force
    if ($running) { $running | Wait-Process -Timeout 10 -ErrorAction SilentlyContinue }
    Write-Output 'OpenOCD stopped; the probe is free for CubeProgrammer.'
    exit 0
}

if ($Flash) {
    Write-Step 'Build'
    $saved = $ErrorActionPreference
    $ErrorActionPreference = 'Continue'
    $buildLog = & $makeExe.FullName -j8 "BUILD_DIR=$BuildDir" 2>&1
    $buildExit = $LASTEXITCODE
    $ErrorActionPreference = $saved
    $buildLog | Select-Object -Last 1 | ForEach-Object { "$_" }
    if ($buildExit -ne 0) {
        $buildLog | Select-String -Pattern 'error' | Select-Object -First 10 | ForEach-Object { "$_" }
        throw 'Build failed'
    }
}

$elf = Join-Path $root "$BuildDir/WSC_Transmitter_Code_V1.elf"
if (-not (Test-Path $elf)) { throw "No ELF at $elf - run with -Flash first" }

Write-Step 'Debug server'
$holder = Get-NetTCPConnection -LocalPort 4444 -State Listen -ErrorAction SilentlyContinue |
    Select-Object -First 1
if ($holder) {
    $holderProc = Get-Process -Id $holder.OwningProcess -ErrorAction SilentlyContinue
    if ($holderProc -and $holderProc.ProcessName -ne 'openocd') {
        throw ("Port 4444 is held by '{0}' (pid {1}), not OpenOCD - stop it and rerun." -f $holderProc.ProcessName, $holder.OwningProcess)
    }
}
$ocd = Get-Process openocd -ErrorAction SilentlyContinue | Where-Object { -not $_.HasExited }
if (-not $holder -and $ocd) {
    foreach ($attempt in 1..20) {
        if (Test-Port4444) { break }
        Start-Sleep -Milliseconds 500
    }
    if (-not (Test-Port4444)) {
        throw ("OpenOCD is running (pid {0}) but nothing answers on 4444 - it may be a session on another probe, or stuck. Run with -StopOpenOcd, then rerun." -f (($ocd | ForEach-Object Id) -join ', '))
    }
}
if (Test-Port4444) {
    Write-Output "OpenOCD already running on 4444 - reusing it."
} else {
    $cli = Get-ChildItem -Path $cubeIde -Filter 'STM32_Programmer_CLI.exe' -Recurse -ErrorAction SilentlyContinue |
        Select-Object -First 1
    if ($cli) {
        $probes = & $cli.FullName -l 2>&1 | Select-String -Pattern 'ST-LINK SN'
        if (-not $probes) { throw 'No ST-LINK detected. Check the USB cable and board power.' }
    }

    $arguments = '-s "{0}" -f interface/stlink-dap.cfg -c "adapter serial {1}" -f target/stm32u5x.cfg' -f $scripts, $ProbeSerial
    Start-Process -FilePath $openocd.FullName -WindowStyle Hidden -ArgumentList $arguments
    $ready = $false
    foreach ($attempt in 1..20) {
        Start-Sleep -Milliseconds 500
        if (Test-Port4444) { $ready = $true; break }
    }
    if (-not $ready) { throw 'OpenOCD did not come up on port 4444' }
    Write-Output 'OpenOCD started.'
}

if ($Flash) {
    Write-Step 'Flash'
    $elfForOcd = ($elf -replace '\\', '/')
    $flashLog = node (Join-Path $PSScriptRoot 'ocd_cmd.js') 'reset halt' "program {$elfForOcd} verify" 'reset run'
    if (($flashLog -join "`n") -match 'Verified OK') {
        Write-Output 'Flashed and verified.'
    } else {
        Write-Output 'Flash reply did not include "Verified OK" - last lines follow; the readback check below decides:'
        $flashLog | Select-Object -Last 10
    }
    Start-Sleep -Seconds 5
}

Write-Step 'Preflight'
$preflightArgs = @((Join-Path $PSScriptRoot 'preflight.js'), $BuildDir)
if ($NoCan) { $preflightArgs += '--no-can' }
node @preflightArgs
exit $LASTEXITCODE
