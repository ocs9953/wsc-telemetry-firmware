#!/bin/bash
set -e
ROLE="$1"; [ -z "$ROLE" ] && { echo "역할 지정 필요: BMS|MPPT|Motor"; exit 1; }
D="${ROLE}_Dummy"
[ -d "$D" ] || { echo "$D 없음"; exit 1; }

TC=/c/ST/STM32CubeIDE_2.2.0/STM32CubeIDE/plugins/com.st.stm32cube.ide.mcu.externaltools.gnu-tools-for-stm32.14.3.rel1.win32_1.0.100.202602081740/tools/bin
GCC=$TC/arm-none-eabi-gcc.exe
SZ=$TC/arm-none-eabi-size.exe
case "$ROLE" in
  BMS)   SN=066DFF555185754867225014 ;;
  MPPT)  SN=066DFF485688494867114316 ;;
  Motor) SN=0667FF545383564867202841 ;;
esac
SN="${WSC_DUMMY_SN:-$SN}"

OUT="$D/build"; mkdir -p "$OUT"

MCU="-mcpu=cortex-m4 -mfpu=fpv4-sp-d16 -mfloat-abi=hard -mthumb"
DEF="-DUSE_HAL_DRIVER -DSTM32F446xx"
INC="-I$D/Core/Inc -I$D/App -Icommon \
 -I$D/Drivers/STM32F4xx_HAL_Driver/Inc \
 -I$D/Drivers/STM32F4xx_HAL_Driver/Inc/Legacy \
 -I$D/Drivers/CMSIS/Device/ST/STM32F4xx/Include \
 -I$D/Drivers/CMSIS/Include"
CFLAGS="$MCU $DEF $INC -Og -g3 -Wall -Wextra -ffunction-sections -fdata-sections"
LD="$D/STM32CubeIDE/STM32F446RETX_FLASH.ld"
LFLAGS="$MCU -T$LD -Wl,--gc-sections -Wl,-Map=$OUT/$D.map --specs=nano.specs --specs=nosys.specs -lm"

SRC="$D/Core/Src/main.c $D/Core/Src/stm32f4xx_hal_msp.c $D/Core/Src/stm32f4xx_it.c \
 $D/Core/Src/system_stm32f4xx.c $D/App/*.c common/*.c \
 $D/Drivers/STM32F4xx_HAL_Driver/Src/*.c \
 $D/STM32CubeIDE/Application/User/Startup/startup_stm32f446retx.s"
SRCLIST=$(ls $SRC 2>/dev/null | grep -v "_template.c")

echo "[BUILD] $D"
$GCC $CFLAGS $SRCLIST $LFLAGS -o "$OUT/$D.elf" -x assembler-with-cpp
$SZ "$OUT/$D.elf"

if [ "$2" = "flash" ]; then
  OCD=/c/ST/STM32CubeIDE_2.2.0/STM32CubeIDE/plugins/com.st.stm32cube.ide.mcu.externaltools.openocd.win32_2.4.500.202604080855/tools/bin/openocd.exe
  SCR="C:/ST/STM32CubeIDE_2.2.0/STM32CubeIDE/plugins/com.st.stm32cube.ide.mcu.debug.openocd_2.3.400.202606220929/resources/openocd/st_scripts"
  echo "[FLASH] $D"
  "$OCD" -s "$SCR" -c "adapter serial $SN" -f interface/stlink-dap.cfg -f target/stm32f4x.cfg \
         -c "program $OUT/$D.elf verify reset exit"
fi
