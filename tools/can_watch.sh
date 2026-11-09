#!/bin/bash
set -u
N="${1:-30}"
[ "$N" = "inf" ] && N=0
OCD=/c/ST/STM32CubeIDE_2.2.0/STM32CubeIDE/plugins/com.st.stm32cube.ide.mcu.externaltools.openocd.win32_2.4.500.202604080855/tools/bin/openocd.exe
SCR="C:/ST/STM32CubeIDE_2.2.0/STM32CubeIDE/plugins/com.st.stm32cube.ide.mcu.debug.openocd_2.3.400.202606220929/resources/openocd/st_scripts"
GDB=/c/ST/STM32CubeIDE_2.2.0/STM32CubeIDE/plugins/com.st.stm32cube.ide.mcu.externaltools.gnu-tools-for-stm32.14.3.rel1.win32_1.0.100.202602081740/tools/bin/arm-none-eabi-gdb.exe
ROOT="$(cd "$(dirname "$0")/.." && pwd)"
ELF="$ROOT/build/debug/WSC_Transmitter_Code_V1.elf"
BOARD_SN="${WSC_BOARD_SN:-7}"

cat > /tmp/cw.gdb <<'EOF'
set pagination off
printf "A_VD %p\n", &VehicleData
printf "A_VDW %u\n", (sizeof(VehicleData)+3)/4
printf "A_TICK %p\n", &uwTick
printf "A_RXN %p\n", &g_mcp_rx_frames
printf "A_RXID %p\n", &g_mcp_rx_last_id
printf "A_TEC %p\n", &g_mcp_tec
printf "A_REC %p\n", &g_mcp_rec
printf "A_MODE %p\n", &g_mcp_opmode
printf "A_TXOK %p\n", &g_vcu_lcd_tx_ok
printf "A_TXNG %p\n", &g_vcu_lcd_tx_fail
printf "A_GAP %p\n", &g_vcu_lcd_gap_last
printf "A_RELAY %p\n", &'vcu_lcd_bridge.c'::to_vcu_last[0]
quit
EOF
eval "$("$GDB" -q -batch -x "$(cygpath -w /tmp/cw.gdb)" "$(cygpath -w "$ELF")" 2>/dev/null \
      | awk '/^A_/{printf "%s=%s\n", substr($1,3), $2}')"
[ -z "${VD:-}" ] && { echo "!! ELF 에서 주소를 못 읽었다: $ELF"; exit 1; }

taskkill //F //IM openocd.exe >/dev/null 2>&1; sleep 1
"$OCD" -s "$SCR" -c "adapter serial $BOARD_SN" -f "$(cygpath -w "$ROOT/openocd.cfg")" >/tmp/ocd.log 2>&1 &
sleep 3
if ! grep -q "Listening on port 4444" /tmp/ocd.log || grep -q "init mode failed" /tmp/ocd.log; then
  echo "!! 타깃 연결 실패 — 서플라이 전원 / ST-Link 연결 확인"
  grep -iE "error|voltage" /tmp/ocd.log | head -4
  taskkill //F //IM openocd.exe >/dev/null 2>&1; exit 1
fi
trap 'taskkill //F //IM openocd.exe >/dev/null 2>&1' EXIT
exec 3<>/dev/tcp/127.0.0.1/4444 || { echo "!! 텔넷 연결 실패"; exit 1; }
timeout 1 cat <&3 >/dev/null 2>&1 || true

i=0
while [ "$N" -eq 0 ] || [ "$i" -lt "$N" ]; do
  printf 'mdw %s %s\nmdw %s 1\nmdw 0x4000a440 2\nmdw 0x42020414 1\nmdw %s 1\nmdw %s 1\nmdw %s 1\nmdw %s 1\nmdw %s 1\nmdw %s 1\nmdw %s 1\nmdw %s 1\nmdw %s 1\necho ENDM\n' \
    "$VD" "$VDW" "$TICK" "$RXN" "$RXID" "$TEC" "$REC" "$MODE" "$TXOK" "$TXNG" "$GAP" "$RELAY" >&3
  RAW=$(timeout 3 sed -n "/ENDM/q;p" <&3 | tr -d "\000")
  if ! printf '%s' "$RAW" | grep -q '^0x'; then
    echo "  !! 타깃 연결 끊김 — 모니터를 멈춘다."; break
  fi
  printf '%s' "$RAW" | awk -v vd="$VD" -v vdw="$VDW" '
    function u16(o,  w) { w = V[int(o/4)]; return (o % 4) ? int(w/65536) : w % 65536 }
    function s16(o,  v) { v = u16(o); return v >= 32768 ? v - 65536 : v }
    function u32(o)     { return V[int(o/4)] }
    function age(t, x)  { return (t > x) ? t - x : 0 }
    function f32(h,  s,e,m,val) {
      s = int(h/2147483648); h = h % 2147483648
      e = int(h/8388608);    m = h % 8388608
      if (e == 0) return 0
      val = (1 + m/8388608.0) * 2 ^ (e-127)
      return s ? -val : val
    }
    BEGIN { B = strtonum(vd); NW = vdw + 0 }
    /^0x[0-9a-fA-F]+: / {
      a = $1; sub(":", "", a); ad = strtonum(a)
      for (j = 2; j <= NF; j++) {
        wa = ad + (j-2)*4; v = strtonum("0x" $j)
        if (wa >= B && wa < B + NW*4) V[(wa-B)/4] = v
        else                         X[n++] = v
      }
    }
    END {
      t=X[0]; ecr=X[1]; psr=X[2]; odr=X[3]
      rxn=X[4]; rxid=X[5]; tec=X[6]; rec=X[7]; mode=X[8]
      txok=X[9]; txng=X[10]; gap=X[11]; relay=X[12]
      printf "\033[H\033[2J"
      printf "  WSC 텔레메트리 — CAN 송수신 모니터        가동 %us\n", int(t/1000)
      printf "  ────────────────────────────────────────────────────────────────────────────────────────────\n"
      printf "  Bus A (더미 3종)   REC=%-3u TEC=%-3u LEC=%u EP=%u BO=%u   고장LED=%u\n",
             int(ecr/256)%128, ecr%256, psr%8, int(psr/32)%2, int(psr/128)%2, int(odr/32768)%2
      printf "  Bus B (LCD)        모드=%s  수신=%-5u 마지막ID=0x%03x  TEC=%-3u REC=%-3u  송신 ok=%-5u ng=%u\n",
             (mode==6?"Normal":(mode==4?"CONFIG!":sprintf("%u",mode))), rxn, rxid, tec, rec, txok, txng
      printf "                     0x501 발행간격=%ums   LCD->VCU 중계 %us 전\n", gap, int(age(t,relay)/1000)
      printf "\n  ── BMS ────────────────────────────────────────────────────────────────────────────────────\n"
      # 오프셋은 types.h 의 BmsData_t (Prohelion, 2026-09) 기준. 구조체를 바꾸면 다시 계산할 것.
      printf "   팩   SOC %5.1f%%   Pack %7.2fV   PackI %7.2fA   Cell %u~%umV  %5.1f~%5.1f℃  상태 0x%02x 프리차지 %u CMU %u대  (%ums 전)\n",
             u16(8)/10.0, u16(0)/100.0, s16(2)/100.0, u16(4), u16(6), s16(10)/10.0, s16(12)/10.0, u16(22)%256, int(u16(20)/256), int(u16(22)/256), age(t,u32(64))
      for (k = 0; k < 4; k++) {
        c = 68 + k*28
        printf "   CMU%d 셀0 %5dmV  셀7 %5dmV  셀온도 %5.1f℃                                            (%ums 전)\n",
               k+1, s16(c+8), s16(c+22), s16(c+6)/10.0, age(t,u32(c+24))
      }
      # ADC(J3) — VehicleData.adc_pot[n] (오프셋 372 + 12n: sum u32, count, min, max, last u16).
      # count/min/max 는 스케줄러가 5초마다 비우므로 "이번 5초 창" 값이다. last 가 실시간 값.
      # mV 는 J3 입력단 환산: raw × 3300 × (470+910) / (16383 × 910). 분압 저항을 바꾸면 같이 고칠 것.
      printf "\n  ── ADC (J3) ───────────────────────────────────────────────────────────────────────────────\n"
      for (k = 0; k < 2; k++) {
        a = 372 + k*12
        cnt = u16(a+4)
        printf "   AIN%d  현재 raw %5u = %5.0fmV   이번 창 %4u샘플  min %5.0fmV  max %5.0fmV%s\n",
               k+1, u16(a+10), u16(a+10)*3300*1380/(16383*910.0), cnt,
               cnt ? u16(a+6)*3300*1380/(16383*910.0) : 0,
               cnt ? u16(a+8)*3300*1380/(16383*910.0) : 0,
               (u16(a+10) >= 16383 ? "   ⚠ 포화" : "")
      }
      printf "\n  ── MPPT ───────────────────────────────────────────────────────────────────────────────────\n"
      for (k = 0; k < 4; k++) {
        o = 180 + k*32
        printf "   #%d   in %6.2fV %6.2fA   out %6.2fV %6.2fA   mode=%u errf=0x%02x  (%ums 전)\n",
               k+1, u16(o)/100.0, u16(o+2)/100.0, u16(o+4)/100.0, u16(o+6)/100.0,
               int(u16(o+12)/256), u16(o+12)%256, age(t,u32(o+28))
      }
      printf "\n  ── Motor ──────────────────────────────────────────────────────────────────────────────────\n"
      m = 308
      pw = int(u16(m) * s16(m+2) / 1000)
      printf "   속도 %6.2f m/s (%5.1f km/h)   rpm %5d   버스 %7.2fV %7.1fA = %6dW\n",
             u16(m+6)/100.0, u16(m+6)*3.6/100.0, s16(m+4), u16(m)/100.0, s16(m+2)/10.0, pw
      printf "   온도 모터 %5.1f℃  방열 %5.1f℃  DSP %5.1f℃   상전류 B %6.1fA C %6.1fA\n",
             s16(m+10)/10.0, s16(m+8)/10.0, s16(m+20)/10.0, s16(m+22)/10.0, s16(m+24)/10.0
      printf "   주행 %9.1fm   limit=0x%04x  error=0x%04x                        (%ums 전)\n",
             f32(u32(360)), u16(m+14), u16(m+12), age(t,u32(356))
      printf "\n  Ctrl+C 로 종료.  코어를 세우지 않으므로 프레임을 유실시키지 않는다.\n"
    }'
  sleep 1
  i=$((i + 1))
done
exec 3<&- 2>/dev/null; exec 3>&- 2>/dev/null
