#!/bin/bash
set -u
ROOT="$(cd "$(dirname "$0")/.." && pwd)"
OCD=/c/ST/STM32CubeIDE_2.2.0/STM32CubeIDE/plugins/com.st.stm32cube.ide.mcu.externaltools.openocd.win32_2.4.500.202604080855/tools/bin/openocd.exe
SCR="C:/ST/STM32CubeIDE_2.2.0/STM32CubeIDE/plugins/com.st.stm32cube.ide.mcu.debug.openocd_2.3.400.202606220929/resources/openocd/st_scripts"
NM=/c/ST/STM32CubeIDE_2.2.0/STM32CubeIDE/plugins/com.st.stm32cube.ide.mcu.externaltools.gnu-tools-for-stm32.14.3.rel1.win32_1.0.100.202602081740/tools/bin/arm-none-eabi-nm.exe
ELF="$ROOT/build/release/WSC_Transmitter_Code_V1.elf"
N="${1:-20}"; GAP="${2:-3}"

[ -f "$ELF" ] || { echo "!! 릴리스 ELF 없음 — .\build.ps1 -Release 먼저"; exit 1; }
a() { "$NM" "$ELF" | awk -v s="$1" '$3==s {print "0x"$1; exit}'; }
A_TICK=$(a uwTick); A_ALIVE=$(a alive_flags)
A_FR=$(a g_uart_rx_frames); A_MSG=$(a g_uart_rx_msgs); A_DROP=$(a g_uart_rx_drops)
A_TEC=$(a g_mcp_tec)
for v in A_TICK A_ALIVE A_FR A_MSG A_DROP; do
  [ -n "${!v}" ] || { echo "!! 심볼 없음: $v — PHASE45 게이트가 꺼진 이미지인지 확인"; exit 1; }
done

taskkill //F //IM openocd.exe >/dev/null 2>&1; sleep 1
"$OCD" -s "$SCR" -c "adapter serial 7" -f "$(cygpath -w "$ROOT/openocd.cfg")" >/tmp/ocd_p45.log 2>&1 &
sleep 3
grep -q "Listening on port 4444" /tmp/ocd_p45.log || {
  echo "!! 연결 실패"; tail -5 /tmp/ocd_p45.log; taskkill //F //IM openocd.exe >/dev/null 2>&1; exit 1; }
exec 3<>/dev/tcp/127.0.0.1/4444 || exit 1
timeout 1 cat <&3 >/dev/null 2>&1 || true
rd() { printf 'mdw %s 1\necho ENDM\n' "$1" >&3
       timeout 2 sed -n "/ENDM/q;p" <&3 | tr -d "\000" | sed -n "s/^0x[0-9a-f]*: *//p" | tr -d ' \r\n'; }

printf "  %-10s %-8s %-8s %-8s %-8s %-8s %s\n" "uwTick" "증가" "alive" "프레임" "메시지" "폐기" "MCP_TEC"
prev=0; resets=0
for i in $(seq 1 "$N"); do
  T=$((0x$(rd $A_TICK))); AL=$(rd $A_ALIVE)
  FR=$((0x$(rd $A_FR))); MS=$((0x$(rd $A_MSG))); DR=$((0x$(rd $A_DROP)))
  TEC=$((0x$(rd $A_TEC)))
  [ -z "$AL" ] && { echo "  !! 끊김"; break; }
  d=$(( T - prev )); [ "$prev" -eq 0 ] && d=0
  [ "$d" -lt 0 ] && { d=0; resets=$((resets+1)); }
  printf "  %-10s %-8s 0x%-6s %-8s %-8s %-8s %s\n" \
     "$T" "$([ $i -gt 1 ] && echo "+$d" || echo '-')" "${AL:4}" "$FR" "$MS" "$DR" "$TEC"
  prev=$T
  [ "$i" -lt "$N" ] && sleep "$GAP"
done
exec 3<&- 2>/dev/null; exec 3>&- 2>/dev/null
taskkill //F //IM openocd.exe >/dev/null 2>&1
echo
echo "  IWDG 리셋 횟수: $resets   <- 0 이어야 정상 (alive 타이밍 회귀 감시)"
echo "  메시지 1건 보낼 때마다: 프레임 +1, 메시지 +1, 폐기 +0 이면 통과"
echo "  MCP_TEC 이 오르면 Bus B 에서 ACK 를 못 받는 것 (LCD/종단 저항 확인)"
