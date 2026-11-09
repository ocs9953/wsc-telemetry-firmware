
set pagination off
set confirm off
target extended-remote localhost:3333
monitor halt

printf "\n===== 시스템 =====\n"
printf "can_ok=%u  mcp_ok=%u  err=%u   uwTick=%u ms\n", SystemState.can_ok, SystemState.mcp_ok, SystemState.error, uwTick

printf "\n===== BMS (Prohelion D1000 Gen1) =====\n"
printf "  SOC=%u permille  Pack=%u cV  PackI=%d (10mA)\n", VehicleData.bms.soc_permille, VehicleData.bms.pack_v_cv, VehicleData.bms.pack_current
printf "  MinCell=%u mV  MaxCell=%u mV  CellTemp=%d~%d (0.1C)\n", VehicleData.bms.min_cell_mv, VehicleData.bms.max_cell_mv, VehicleData.bms.min_cell_temp, VehicleData.bms.max_cell_temp
printf "  ext_status=0x%08x  status=0x%02x  contactor=0x%02x  precharge=%u  cmu_count=%u\n", VehicleData.bms.ext_status, VehicleData.bms.status_flags, VehicleData.bms.contactor_status, VehicleData.bms.precharge_state, VehicleData.bms.cmu_count
printf "  last_tick=%u   (uwTick 과의 차 = 마지막 수신 후 경과 ms)\n", VehicleData.bms.bms_last_tick
printf "  CMU1 sn=0x%08x cell0..3 = %d %d %d %d  T=%d  tick=%u\n", VehicleData.bms.cmu[0].serial, VehicleData.bms.cmu[0].cell_mv[0], VehicleData.bms.cmu[0].cell_mv[1], VehicleData.bms.cmu[0].cell_mv[2], VehicleData.bms.cmu[0].cell_mv[3], VehicleData.bms.cmu[0].cell_temp, VehicleData.bms.cmu[0].last_tick
printf "  CMU2 sn=0x%08x cell0=%d  T=%d  tick=%u\n", VehicleData.bms.cmu[1].serial, VehicleData.bms.cmu[1].cell_mv[0], VehicleData.bms.cmu[1].cell_temp, VehicleData.bms.cmu[1].last_tick
printf "  CMU3 sn=0x%08x cell0=%d  T=%d  tick=%u\n", VehicleData.bms.cmu[2].serial, VehicleData.bms.cmu[2].cell_mv[0], VehicleData.bms.cmu[2].cell_temp, VehicleData.bms.cmu[2].last_tick
printf "  CMU4 sn=0x%08x cell0=%d  T=%d  tick=%u\n", VehicleData.bms.cmu[3].serial, VehicleData.bms.cmu[3].cell_mv[0], VehicleData.bms.cmu[3].cell_temp, VehicleData.bms.cmu[3].last_tick

printf "\n===== MPPT 1~4 =====\n"
printf "  [0] inV=%u inI=%u outV=%u outI=%u  mode=%u errf=0x%02x  tick=%u\n", VehicleData.mppt[0].in_v, VehicleData.mppt[0].in_i, VehicleData.mppt[0].out_v, VehicleData.mppt[0].out_i, VehicleData.mppt[0].mode, VehicleData.mppt[0].error_flags, VehicleData.mppt[0].mppt_last_tick
printf "  [1] inV=%u inI=%u outV=%u outI=%u  mode=%u errf=0x%02x  tick=%u\n", VehicleData.mppt[1].in_v, VehicleData.mppt[1].in_i, VehicleData.mppt[1].out_v, VehicleData.mppt[1].out_i, VehicleData.mppt[1].mode, VehicleData.mppt[1].error_flags, VehicleData.mppt[1].mppt_last_tick
printf "  [2] inV=%u inI=%u outV=%u outI=%u  mode=%u errf=0x%02x  tick=%u\n", VehicleData.mppt[2].in_v, VehicleData.mppt[2].in_i, VehicleData.mppt[2].out_v, VehicleData.mppt[2].out_i, VehicleData.mppt[2].mode, VehicleData.mppt[2].error_flags, VehicleData.mppt[2].mppt_last_tick
printf "  [3] inV=%u inI=%u outV=%u outI=%u  mode=%u errf=0x%02x  tick=%u   <-- S5 (0x340~0x346, 필터 상한 검증)\n", VehicleData.mppt[3].in_v, VehicleData.mppt[3].in_i, VehicleData.mppt[3].out_v, VehicleData.mppt[3].out_i, VehicleData.mppt[3].mode, VehicleData.mppt[3].error_flags, VehicleData.mppt[3].mppt_last_tick

printf "\n===== Motor =====\n"
printf "  BusV=%u (0.01V)  BusI=%d (0.1A, 회생 시 음수)  rpm=%d  speed=%u (0.01 m/s)\n", VehicleData.motor.bus_v, VehicleData.motor.bus_i, VehicleData.motor.motor_rpm, VehicleData.motor.veh_speed
printf "  MotorT=%d  HeatsinkT=%d  DspT=%d  (0.1C)\n", VehicleData.motor.motor_temp, VehicleData.motor.heatsink_temp, VehicleData.motor.dsp_temp
printf "  PhaseB=%d PhaseC=%d (0.1A)  active_motor=%u\n", VehicleData.motor.phase_b_i, VehicleData.motor.phase_c_i, VehicleData.motor.active_motor
printf "  limit=0x%04x  error=0x%04x   <-- S2 (sticky OR 누적, 0x0004 가 보이면 통과)\n", VehicleData.motor.limit_flags, VehicleData.motor.error_flags
printf "  odo=%f m  bus_ah=%f  odo_last_tick=%u\n", VehicleData.distance_traveled, VehicleData.battery_accumulated_energy, VehicleData.odo_last_tick
printf "  motor_last_tick=%u\n", VehicleData.motor.motor_last_tick


printf "\n===== Bus A (FDCAN1) 버스 진단 =====\n"
printf "  CCCR=0x%08x   INIT=%u CCE=%u MON=%u TEST=%u\n", hfdcan1.Instance->CCCR, (hfdcan1.Instance->CCCR>>0)&1, (hfdcan1.Instance->CCCR>>1)&1, (hfdcan1.Instance->CCCR>>5)&1, (hfdcan1.Instance->CCCR>>7)&1
printf "    -> INIT=1 이면 주변장치가 초기화 모드라 수신 자체를 안 한다\n"
printf "  ECR =0x%08x   TEC=%u  REC=%u  RP=%u  CEL=%u\n", hfdcan1.Instance->ECR, hfdcan1.Instance->ECR&0xFF, (hfdcan1.Instance->ECR>>8)&0x7F, (hfdcan1.Instance->ECR>>15)&1, (hfdcan1.Instance->ECR>>16)&0xFF
printf "    -> TEC/REC 가 계속 오르면 배선/비트레이트/종단 문제\n"
printf "  PSR =0x%08x   LEC=%u  ACT=%u  EP=%u EW=%u BO=%u\n", hfdcan1.Instance->PSR, hfdcan1.Instance->PSR&7, (hfdcan1.Instance->PSR>>3)&3, (hfdcan1.Instance->PSR>>5)&1, (hfdcan1.Instance->PSR>>6)&1, (hfdcan1.Instance->PSR>>7)&1
printf "    LEC: 0=없음 1=Stuff 2=Form 3=Ack 4=Bit1 5=Bit0 6=CRC 7=미변경\n"
printf "         3(Ack) = 아무도 응답 안 함(상대 노드 꺼짐/종단 없음)\n"
printf "         1(Stuff)/6(CRC) = 비트레이트 불일치 의심\n"
printf "    ACT: 0=동기화중 1=대기 2=수신중 3=송신중\n"
printf "    BO=1 이면 버스오프. AutoBusOff 로 자동 복구되지만 원인은 남아 있다\n"
printf "  IR  =0x%08x   RF0L(FIFO0 유실)=%u  RF0F(가득참)=%u\n", hfdcan1.Instance->IR, (hfdcan1.Instance->IR>>3)&1, (hfdcan1.Instance->IR>>2)&1
printf "    -> RF0L=1 이면 프레임이 실제로 왔는데 처리 전에 덮였다(ISR 지연)\n"
printf "  RXF0S=0x%08x  큐잉된 개수=%u (FIFO0 는 하드웨어 고정 3칸)\n", hfdcan1.Instance->RXF0S, hfdcan1.Instance->RXF0S&0x7F

printf "\n===== Bus B (MCP2518FD) 상태 =====\n"
printf "  mcp_initialized=%u  mcp_asleep=%u  last_activity=%u ms\n", 'ext_can_controller.c'::mcp_initialized, 'ext_can_controller.c'::mcp_asleep, 'ext_can_controller.c'::mcp_last_activity_ms
printf "    -> mcp_asleep=1 이 정상(유휴). 버스 활동이 오면 깨어난다\n"
printf "\n"
monitor resume
detach
quit
