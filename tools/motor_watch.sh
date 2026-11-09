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

ELF_W="$(cygpath -w "$ELF")"
CFG_W="$(cygpath -w "$ROOT/openocd.cfg")"

cat > /tmp/addr.gdb <<'EOF'
set pagination off
printf "BASE %p\n", &VehicleData.motor
printf "TICK %p\n", &uwTick
quit
EOF
eval "$("$GDB" -q -batch -x "$(cygpath -w /tmp/addr.gdb)" "$ELF_W" 2>/dev/null \
       | awk '/^BASE/{printf "BASE=%s\n",$2} /^TICK/{printf "TICK=%s\n",$2}')"
[ -z "${BASE:-}" ] && { echo "!! ELF 에서 주소를 못 읽었다: $ELF"; exit 1; }

if netstat -ano 2>/dev/null | grep -q ":4444 .*LISTENING"; then
  echo "!! 이미 다른 모니터/디버그 세션이 ST-Link 를 쓰고 있다(4444 포트 사용 중)."
  echo "   그 창을 먼저 닫을 것 — 여기서 openocd 를 죽이면 그쪽이 끊긴다."
  exit 1
fi
taskkill //F //IM openocd.exe >/dev/null 2>&1; sleep 1
"$OCD" -s "$SCR" -c "adapter serial $BOARD_SN" -f "$CFG_W" >/tmp/ocd.log 2>&1 &
sleep 2
if ! grep -q "Listening on port 4444" /tmp/ocd.log; then
  echo "!! OpenOCD 연결 실패 — 보드 전원/ST-Link 확인"
  grep -iE "error|voltage" /tmp/ocd.log | head -4
  taskkill //F //IM openocd.exe >/dev/null 2>&1; exit 1
fi
trap 'taskkill //F //IM openocd.exe >/dev/null 2>&1' EXIT

printf "  시각(ms)   속도             rpm   버스전압  버스전류    전력   모터온 방열  DSP   상B      상C      주행거리  limit  err   수신\n"
printf "  %s\n" "$(printf '─%.0s' $(seq 1 122))"

exec 3<>/dev/tcp/127.0.0.1/4444 || { echo "!! 텔넷 연결 실패"; exit 1; }
timeout 1 cat <&3 >/dev/null 2>&1 || true

i=0
while [ "$N" -eq 0 ] || [ "$i" -lt "$N" ]; do
  printf 'mdw %s 14\nmdw %s 1\necho ENDMARK\n' "$BASE" "$TICK" >&3
  RAW=$(timeout 2 sed -n "/ENDMARK/q;p" <&3 | tr -d "\000")
  if ! printf '%s' "$RAW" | grep -q '^0x'; then
    echo "  !! 타깃 연결 끊김 — OpenOCD 종료 또는 ST-Link 분리. 모니터를 멈춘다."
    break
  fi
  echo "$RAW" | awk -v ba="$BASE" -v ta="$TICK" '
    function s16(v) { return v >= 32768 ? v - 65536 : v }
    function f32(h,   s,e,m,val) {          # IEEE754 단정밀도 디코드
      s = int(h / 2147483648); h = h % 2147483648
      e = int(h / 8388608);    m = h % 8388608
      if (e == 0) return 0
      val = (1 + m / 8388608.0) * 2 ^ (e - 127)
      return s ? -val : val
    }
    BEGIN { B = strtonum(ba); TA = strtonum(ta) }
    # mdw 는 한 줄에 4~8 워드씩 나눠 찍는다. 줄 단위가 아니라
    # **워드별 실주소**로 배치해야 한다(이걸 놓쳐서 처음에 오독했다).
    /^0x[0-9a-fA-F]+: / {
      addr = $1; sub(":", "", addr); a = strtonum(addr)
      for (j = 2; j <= NF; j++) {
        wa = a + (j - 2) * 4
        v  = strtonum("0x" $j)
        if (wa == TA)                        T = v
        else if (wa >= B && wa < B + 56)     W[(wa - B) / 4] = v
      }
    }
    END {
      bus_v = W[0] % 65536;      bus_i  = s16(int(W[0] / 65536))
      rpm   = s16(W[1] % 65536); speed  = int(W[1] / 65536)
      heat  = s16(W[2] % 65536); mtemp  = s16(int(W[2] / 65536))
      errf  = W[3] % 65536;      limitf = int(W[3] / 65536)
      dsp   = s16(W[5] % 65536); phb    = s16(int(W[5] / 65536))
      phc   = s16(W[6] % 65536)
      tick  = W[12];             dist   = f32(W[13])
      pw    = int(bus_v * bus_i / 1000)
      printf "  %8u  %6.2f m/s %6.1fkm/h %5d  %7.2fV %7.1fA %7dW  %5.1f %5.1f %5.1f %6.1fA %7.1fA %9.1fm 0x%04x 0x%04x %5ums\n",
             T, speed/100.0, speed*3.6/100.0, rpm, bus_v/100.0, bus_i/10.0, pw,
             mtemp/10.0, heat/10.0, dsp/10.0, phb/10.0, phc/10.0, dist, limitf, errf, T - tick
    }'
  sleep 1
  i=$((i + 1))
done
exec 3<&- 2>/dev/null; exec 3>&- 2>/dev/null

echo
echo "  코어를 정지시키지 않았으므로 이 모니터는 프레임을 유실시키지 않는다."
echo "  '수신' = 마지막 모터 프레임 이후 경과. 계속 작으면 정상 수신 중."
