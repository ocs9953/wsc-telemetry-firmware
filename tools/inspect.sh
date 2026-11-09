#!/bin/bash
set -u
OCD=/c/ST/STM32CubeIDE_2.2.0/STM32CubeIDE/plugins/com.st.stm32cube.ide.mcu.externaltools.openocd.win32_2.4.500.202604080855/tools/bin/openocd.exe
SCR="C:/ST/STM32CubeIDE_2.2.0/STM32CubeIDE/plugins/com.st.stm32cube.ide.mcu.debug.openocd_2.3.400.202606220929/resources/openocd/st_scripts"
GDB=/c/ST/STM32CubeIDE_2.2.0/STM32CubeIDE/plugins/com.st.stm32cube.ide.mcu.externaltools.gnu-tools-for-stm32.14.3.rel1.win32_1.0.100.202602081740/tools/bin/arm-none-eabi-gdb.exe
ROOT="$(cd "$(dirname "$0")/.." && pwd)"

BOARD_SN="${WSC_BOARD_SN:-7}"

NM="$(dirname "$GDB")/arm-none-eabi-nm.exe"
ELF_REL="${WSC_ELF:-$ROOT/build/release/WSC_Transmitter_Code_V1.elf}"
ELF_DBG="$ROOT/build/debug/WSC_Transmitter_Code_V1.elf"

sym() {
  "$NM" "$ELF_REL" 2>/dev/null | awk -v n="$1" '$3==n {print "0x"$1; exit}'
}

check_elf() {
  [ -f "$ELF_REL" ] || { echo "!! 구운 ELF 가 없다: $ELF_REL"; exit 1; }
  if [ -f "$ELF_DBG" ] && [ "$ELF_DBG" -ot "$ELF_REL" ]; then
    echo "  (참고) debug ELF 가 release 보다 오래됐다 — 구조체가 바뀌었다면 오프셋이 어긋날 수 있다."
  fi
}


run() {
  taskkill //F //IM openocd.exe >/dev/null 2>&1; sleep 1
  "$OCD" -s "$SCR" -c "adapter serial $BOARD_SN" $1 -c "gdb_port 3333" >/tmp/ocd.log 2>&1 &
  sleep 2
  if ! grep -q "Listening on port 3333" /tmp/ocd.log; then
    echo "!! OpenOCD 연결 실패 — 보드 전원/ST-Link 확인"
    grep -iE "error|voltage" /tmp/ocd.log | head -4; return 1
  fi
  "$GDB" -q -batch -x "$3" "$2" 2>&1 | grep -vE "^\[Inferior|^Info :|^0x00000000 in|^xPSR:"
  taskkill //F //IM openocd.exe >/dev/null 2>&1
}

case "${1:-board}" in
  board)
    run "-f $ROOT/openocd.cfg" "$ROOT/build/debug/WSC_Transmitter_Code_V1.elf" "$ROOT/tools/phase2_inspect.gdb"
    ;;
  watch)
    check_elf
    cat > /tmp/aw.gdb <<'EOF'
set pagination off
printf "O_CANOK %d\n", (int)((char*)&SystemState.can_ok - (char*)&SystemState)
printf "O_BMS %d\n", (int)((char*)&VehicleData.bms.bms_last_tick - (char*)&VehicleData)
printf "O_MPPT %d\n", (int)((char*)&VehicleData.mppt[0].mppt_last_tick - (char*)&VehicleData)
printf "O_MOTOR %d\n", (int)((char*)&VehicleData.motor.motor_last_tick - (char*)&VehicleData)
quit
EOF
    eval "$("$GDB" -q -batch -x "$(cygpath -w /tmp/aw.gdb)" \
            "$(cygpath -w "$ELF_DBG")" 2>/dev/null \
          | awk '/^O_/{printf "%s=%s\n", $1, $2}')"
    B_SS=$(sym SystemState); B_VD=$(sym VehicleData); TICK=$(sym uwTick)
    if [ -z "$TICK" ] || [ -z "$B_SS" ] || [ -z "$B_VD" ] || [ -z "${O_BMS:-}" ]; then
      echo "!! 주소를 못 풀었다 (release 심볼 또는 debug 오프셋 누락)"; exit 1
    fi
    CANOK=$(printf '0x%x' $(( B_SS + O_CANOK )))
    BMS=$(printf   '0x%x' $(( B_VD + O_BMS   )))
    MPPT=$(printf  '0x%x' $(( B_VD + O_MPPT  )))
    MOTOR=$(printf '0x%x' $(( B_VD + O_MOTOR )))

    taskkill //F //IM openocd.exe >/dev/null 2>&1; sleep 1
    "$OCD" -s "$SCR" -c "adapter serial $BOARD_SN" -f "$(cygpath -w "$ROOT/openocd.cfg")" >/tmp/ocd.log 2>&1 &
    sleep 3
    if ! grep -q "Listening on port 4444" /tmp/ocd.log \
       || grep -q "init mode failed" /tmp/ocd.log; then
      echo "!! 타깃 연결 실패 — 서플라이 전원 / ST-Link 연결을 확인할 것"
      grep -iE "error|voltage" /tmp/ocd.log | head -4
      taskkill //F //IM openocd.exe >/dev/null 2>&1; exit 1
    fi
    exec 3<>/dev/tcp/127.0.0.1/4444 || {
      echo "!! 텔넷 연결 실패"; taskkill //F //IM openocd.exe >/dev/null 2>&1; exit 1; }
    timeout 1 cat <&3 >/dev/null 2>&1 || true

    for i in 1 2 3; do
      printf 'mdw %s 1\nmdw %s 1\nmdw %s 1\nmdw %s 1\nmdw 0x4000a440 2\nmdw 0x42020414 1\nmdb %s 1\necho ENDM\n' \
             "$TICK" "$BMS" "$MPPT" "$MOTOR" "$CANOK" >&3
      RAW=$(timeout 2 sed -n "/ENDM/q;p" <&3 | tr -d "\000")
      if ! printf '%s' "$RAW" | grep -q '^0x'; then
        echo "  !! 타깃 연결 끊김 — 점검을 멈춘다."
        break
      fi
      printf '%s' "$RAW" | awk -v n="$i" '
        /^0x[0-9a-f]+: / {
          a = $1; sub(":", "", a)
          for (j = 2; j <= NF; j++) V[k++] = strtonum("0x" $j)
        }
        # 코어를 세우지 않고 읽으므로 uwTick 과 각 tick 사이에 타깃이 계속
        # 진행한다. tick 이 uwTick 스냅샷보다 새로우면 뺄셈이 음수가 되어
        # unsigned 로 뒤집히므로 0 으로 눌러 준다.
        function age(t, x) { return (t > x) ? t - x : 0 }
        END {
          t=V[0]; bms=V[1]; mppt=V[2]; motor=V[3]; ecr=V[4]; psr=V[5]; odr=V[6]; ss=V[7]
          printf "  [%d] LED_CAN=%u can_ok=%u | REC=%3u TEC=%3u LEC=%u EP=%u BO=%u | 마지막수신경과(ms) BMS=%u MPPT=%u MOTOR=%u\n",
                 n, int(odr/32768)%2, ss%256,
                 int(ecr/256)%128, ecr%256, psr%8, int(psr/32)%2, int(psr/128)%2,
                 age(t,bms), age(t,mppt), age(t,motor)
        }'
      [ $i -lt 3 ] && sleep 4
    done
    exec 3<&- 2>/dev/null; exec 3>&- 2>/dev/null
    taskkill //F //IM openocd.exe >/dev/null 2>&1
    echo
    echo "  경과가 uwTick 만큼 크면 = 그 노드는 한 번도 수신 안 됨"
    echo "  코어를 세우지 않으므로 이 점검은 프레임을 유실시키지 않는다."
    ;;
  busb)
    check_elf
    TICK=$(sym uwTick)
    SLEEP=$(sym mcp_asleep)
    ACT=$(sym mcp_last_activity_ms)
    TOVCU=$(sym to_vcu_last)
    TOLCD=$(sym to_lcd_last)
    if [ -z "$TICK" ] || [ -z "$SLEEP" ] || [ -z "$ACT" ] || [ -z "$TOVCU" ] || [ -z "$TOLCD" ]; then
      echo "!! ELF 심볼표에서 주소를 못 읽었다: $ELF_REL"; exit 1
    fi

    taskkill //F //IM openocd.exe >/dev/null 2>&1; sleep 1
    "$OCD" -s "$SCR" -c "adapter serial $BOARD_SN" -f "$(cygpath -w "$ROOT/openocd.cfg")" >/tmp/ocd.log 2>&1 &
    sleep 3
    if ! grep -q "Listening on port 4444" /tmp/ocd.log \
       || grep -q "init mode failed" /tmp/ocd.log; then
      echo "!! 타깃 연결 실패 — 서플라이 전원 / ST-Link 연결을 확인할 것"
      grep -iE "error|voltage" /tmp/ocd.log | head -4
      taskkill //F //IM openocd.exe >/dev/null 2>&1; exit 1
    fi
    exec 3<>/dev/tcp/127.0.0.1/4444 || { echo "!! 텔넷 연결 실패"; exit 1; }
    timeout 1 cat <&3 >/dev/null 2>&1 || true

    for i in 1 2 3; do
      printf 'mdw %s 1\nmdw %s 1\nmdb %s 1\nmdw %s 1\nmdw %s 1\necho ENDM\n' \
             "$TICK" "$ACT" "$SLEEP" "$TOVCU" "$TOLCD" >&3
      RAW=$(timeout 2 sed -n "/ENDM/q;p" <&3 | tr -d "\000")
      if ! printf '%s' "$RAW" | grep -q '^0x'; then
        echo "  !! 타깃 연결 끊김 — 점검을 멈춘다."; break
      fi
      printf '%s' "$RAW" | awk -v n="$i" '
        /^0x[0-9a-f]+: / { for (j = 2; j <= NF; j++) V[k++] = strtonum("0x" $j) }
        function age(t, x) { return (t > x) ? t - x : 0 }
        END {
          t=V[0]; act=V[1]; slp=V[2]; tovcu=V[3]; tolcd=V[4]
          # VcuLcd_Init() 이 부팅 시 to_vcu_last/to_lcd_last 를 HAL_GetTick() 으로
          # 채운다(첫 순회에 전 항목이 몰려 나가는 것을 막기 위해). 따라서 "0 이면
          # 기록 없음" 판정은 성립하지 않는다. 경과가 가동시간에 육박하면
          # 부팅 이후 한 번도 없었다는 뜻이다.
          printf "  [%d] uwTick=%us | mcp_asleep=%u | BusB 마지막활동=%ums | LCD->VCU 중계=%ums | 보드->LCD 발행=%ums\n",
                 n, int(t/1000), slp, age(t,act), age(t,tovcu), age(t,tolcd)
        }'
      [ $i -lt 3 ] && sleep 4
    done
    exec 3<&- 2>/dev/null; exec 3>&- 2>/dev/null
    taskkill //F //IM openocd.exe >/dev/null 2>&1
    echo
    echo "  'BusB 마지막활동' 이 계속 작으면 LCD 가 송신 중이라는 뜻이다."
    echo "  'LCD->VCU 중계' 경과가 uwTick 에 육박하면 0x510~0x51F 명령이 아직 한 번도 안 온 것이다."
    echo "  '보드->LCD 발행' 은 0x501(200ms 주기) 송신 시각. 계속 200ms 이하면 정상."
    ;;
  dummy)
    R="${2:-BMS}"
    run "-f interface/stlink-dap.cfg -f target/stm32f4x.cfg" \
        "$ROOT/tests/dummy_nodes/${R}_Dummy/build/${R}_Dummy.elf" \
        "$ROOT/tests/dummy_nodes/dummy_inspect.gdb"
    ;;
  *) echo "사용법: $0 [board|watch|busb|dummy BMS|MPPT|Motor]"; exit 1 ;;
esac
