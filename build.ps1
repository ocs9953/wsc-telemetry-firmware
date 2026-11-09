# STM32 Build Script
# Patches STM32Make.make to work around STM32 for VSCode extension bugs:
# 1) CXX_SOURCES = \ empty line issue
# 2) @echo -> $(file >) conversion (Windows cmd 8191 char limit workaround)

param(
    [switch]$Clean,
    [switch]$Flash,
    [switch]$Release
)

$makeFile = "STM32Make.make"
if (Test-Path $makeFile) {
    $content = Get-Content $makeFile -Raw
    $patched = $false

    # Patch 1: CXX_SOURCES = \ (trailing backslash) -> CXX_SOURCES =
    if ($content -match 'CXX_SOURCES\s*=\s*\\(\r?\n)') {
        $content = $content -replace 'CXX_SOURCES\s*=\s*\\(\r?\n)', "CXX_SOURCES =`$1"
        $patched = $true
        Write-Host "[PATCH] CXX_SOURCES backslash removed" -ForegroundColor Green
    }

    # Patch 2: @echo $(OBJECTS) > $@.in -> $(file >$@.in,$(OBJECTS))
    # This avoids Windows cmd 8191 char limit that truncates the object list
    if ($content -match '@echo\s+\$\((VALID_OBJECTS|OBJECTS)\)\s*>\s*\$@\.in') {
        $content = $content -replace '@echo\s+\$\((VALID_OBJECTS|OBJECTS)\)\s*>\s*\$@\.in', '$(file >$@.in,$(OBJECTS))'
        $patched = $true
        Write-Host "[PATCH] echo -> file function (cmd length limit workaround)" -ForegroundColor Green
    }

    if ($patched) {
        [System.IO.File]::WriteAllText($makeFile, $content)
    } else {
        Write-Host "[OK] STM32Make.make no patch needed" -ForegroundColor Cyan
    }
} else {
    Write-Host "[WARN] STM32Make.make not found" -ForegroundColor Yellow
}

# Remove old .elf.in cache
$elfInFiles = Get-ChildItem -Path "build" -Filter "*.elf.in" -Recurse -ErrorAction SilentlyContinue
foreach ($f in $elfInFiles) {
    Remove-Item $f.FullName -Force
    Write-Host "[CLEAN] $($f.FullName) deleted" -ForegroundColor Yellow
}

# Clean build
if ($Clean) {
    Write-Host "[CLEAN] Removing build folder..." -ForegroundColor Yellow
    if (Test-Path "build") { Remove-Item -Recurse -Force "build" }
}

foreach ($d in @("release", "debug")) {
    $o = "build/$d/rtc.o"
    if (Test-Path $o) { Remove-Item -Force $o }
}

# Run make
Write-Host ""
$makePath = "C:\ST\STM32CubeIDE_2.2.0\STM32CubeIDE\plugins\com.st.stm32cube.ide.mcu.externaltools.make.win32_2.2.200.202604021615\tools\bin\make.exe"

if ($Release) {
    Write-Host "[BUILD] Running make (RELEASE: DEBUG=0, -O2)..." -ForegroundColor White
    & $makePath -j16 DEBUG=0 OPTIMIZATION=-O2 -f STM32Make.make
    $buildDir = "release"
} else {
    Write-Host "[BUILD] Running make (DEBUG: -Og)..." -ForegroundColor White
    & $makePath -j16 DEBUG=1 -f STM32Make.make
    $buildDir = "debug"
}

if ($LASTEXITCODE -ne 0) {
    Write-Host "[ERROR] Build failed" -ForegroundColor Red
    exit 1
}

# Flash
if ($Flash) {
    Write-Host ""
    Write-Host "[FLASH] Programming MCU ($buildDir)..." -ForegroundColor Cyan
    $openocdPath = "C:\ST\STM32CubeIDE_2.2.0\STM32CubeIDE\plugins\com.st.stm32cube.ide.mcu.externaltools.openocd.win32_2.4.500.202604080855\tools\bin\openocd.exe"
    $elfPath = "build/$buildDir/WSC_Transmitter_Code_V1.elf"
    $ocdScripts = "C:\ST\STM32CubeIDE_2.2.0\STM32CubeIDE\plugins\com.st.stm32cube.ide.mcu.debug.openocd_2.3.400.202606220929\resources\openocd\st_scripts"
    $boardSn = if ($env:WSC_BOARD_SN) { $env:WSC_BOARD_SN } else { "7" }
    & $openocdPath -s $ocdScripts -c "adapter serial $boardSn" -f ./openocd.cfg -c "program $elfPath verify reset exit"
}
