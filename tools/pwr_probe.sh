#!/bin/bash
set -u
ROOT="$(cd "$(dirname "$0")/.." && pwd)"
OCD=/c/ST/STM32CubeIDE_2.2.0/STM32CubeIDE/plugins/com.st.stm32cube.ide.mcu.externaltools.openocd.win32_2.4.500.202604080855/tools/bin/openocd.exe
SCR="C:/ST/STM32CubeIDE_2.2.0/STM32CubeIDE/plugins/com.st.stm32cube.ide.mcu.debug.openocd_2.3.400.202606220929/resources/openocd/st_scripts"
PROBE="${1:-7}"

taskkill //F //IM openocd.exe >/dev/null 2>&1; sleep 1
"$OCD" -s "$SCR" -c "adapter serial $PROBE" -f "$(cygpath -w "$ROOT/openocd.cfg")" >/tmp/ocd_pwr.log 2>&1 &
sleep 3
grep -q "Listening on port 4444" /tmp/ocd_pwr.log || {
  echo "!! OpenOCD 연결 실패"; tail -5 /tmp/ocd_pwr.log; taskkill //F //IM openocd.exe >/dev/null 2>&1; exit 1; }
exec 3<>/dev/tcp/127.0.0.1/4444 || { taskkill //F //IM openocd.exe >/dev/null 2>&1; exit 1; }
timeout 1 cat <&3 >/dev/null 2>&1 || true

cmd() { printf '%s\necho ENDM\n' "$1" >&3; timeout 3 sed -n "/ENDM/q;p" <&3 | tr -d '\000\r'; }
rd()  { cmd "mdw $1 1" | sed -n "s/^0x[0-9a-f]*: *//p" | tr -d ' \n'; }

show() {
  local cr3=$((0x$(rd 0x46020808))) svm=$((0x$(rd 0x4602083C)))
  local bd=$((0x$(rd 0x46020820)))  vos=$((0x$(rd 0x4602080C)))
  printf "    CR3   = 0x%08X  REGSEL=%d  선택      : %s\n" $cr3 $(((cr3>>1)&1)) \
         "$([ $(((cr3>>1)&1)) -eq 1 ] && echo SMPS || echo LDO)"
  printf "    SVMSR = 0x%08X  REGS  =%d  실동작    : %s   ACTVOS=%d\n" $svm $(((svm>>1)&1)) \
         "$([ $(((svm>>1)&1)) -eq 1 ] && echo SMPS || echo LDO)" $(((svm>>16)&3))
  printf "    BDCR1 = 0x%08X  BREN  =%d  백업SRAM : %s\n" $bd $((bd&1)) \
         "$([ $((bd&1)) -eq 1 ] && echo 유지 || echo 미유지)"
  printf "    VOSR  = 0x%08X  VOS   =%d\n" $vos $(((vos>>16)&3))
}

echo "  [1] 실행 중인 펌웨어 (halt 안 함)"; show
echo
echo "  [2] reset halt 직후 — SystemPower_Config() 실행 전"
cmd "reset halt" >/dev/null; show
echo
echo "      -> [2] 의 REGSEL 이 0(LDO) 이면 리셋 기본값이 LDO 임이 확정된다."
echo
echo "  [3] 실행 재개"
cmd "resume" >/dev/null; sleep 1; show

exec 3<&- 2>/dev/null; exec 3>&- 2>/dev/null
taskkill //F //IM openocd.exe >/dev/null 2>&1
