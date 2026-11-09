
set pagination off
set confirm off
target extended-remote localhost:3333
monitor halt

set $rcc = (RCC_TypeDef *) 0x40023800

printf "\n===== 지금 어디서 돌고 있나 =====\n"
printf "  PC = 0x%08x\n", $pc
info symbol $pc
printf "  -> Error_Handler 로 나오면 부팅 실패다(HSE 미기동 등). 그 경우\n"
printf "     CAN 프레임은 하나도 나가지 않으므로 버스 트래픽의 출처가 아니다.\n"

printf "\n===== 클럭 =====\n"
printf "  CR   = 0x%08x   HSION=%u HSIRDY=%u  HSEON=%u HSERDY=%u HSEBYP=%u  PLLON=%u PLLRDY=%u\n", $rcc->CR, $rcc->CR&1, ($rcc->CR>>1)&1, ($rcc->CR>>16)&1, ($rcc->CR>>17)&1, ($rcc->CR>>18)&1, ($rcc->CR>>24)&1, ($rcc->CR>>25)&1
printf "  CFGR = 0x%08x   SW=%u SWS=%u  HPRE=0x%x PPRE1=0x%x\n", $rcc->CFGR, $rcc->CFGR&3, ($rcc->CFGR>>2)&3, ($rcc->CFGR>>4)&0xF, ($rcc->CFGR>>10)&7
printf "    SWS: 0=HSI(16MHz)  1=HSE  2=PLL   <-- 실제로 무엇이 SYSCLK 인가\n"
printf "    HSERDY=0 인데 SWS=0 이면 HSE 가 안 떠서 HSI 로 도는 것이다.\n"
printf "  SystemCoreClock = %u Hz   (HAL 이 믿고 있는 값. 실제와 다를 수 있다)\n", SystemCoreClock

printf "\n===== CAN1 비트타이밍 =====\n"
set $can = (CAN_TypeDef *) 0x40006400
printf "  BTR  = 0x%08x   BRP=%u  TS1=%u TS2=%u SJW=%u  (LBKM=%u SILM=%u)\n", $can->BTR, ($can->BTR&0x3FF)+1, (($can->BTR>>16)&0xF)+1, (($can->BTR>>20)&7)+1, (($can->BTR>>24)&3)+1, ($can->BTR>>30)&1, ($can->BTR>>31)&1
printf "    총 TQ = 1 + TS1 + TS2 = %u\n", 1 + (($can->BTR>>16)&0xF)+1 + (($can->BTR>>20)&7)+1
printf "    비트레이트 = APB1 / (BRP x 총TQ).  APB1 = SYSCLK / PPRE1분주\n"
printf "    SYSCLK 8MHz, BRP 1, 16TQ 이면 500kbps 가 맞다.\n"
printf "    SYSCLK 16MHz(HSI) 로 돌고 있으면 1Mbps 가 되어 전부 에러가 난다.\n"
printf "  MSR  = 0x%08x   ESR = 0x%08x\n", $can->MSR, $can->ESR
printf "    ESR: LEC=%u  BOFF=%u EPVF=%u EWGF=%u  TEC=%u REC=%u\n", ($can->ESR>>4)&7, ($can->ESR>>2)&1, ($can->ESR>>1)&1, $can->ESR&1, ($can->ESR>>16)&0xFF, ($can->ESR>>24)&0xFF
printf "    TEC 가 크면 이 더미가 송신은 하는데 아무도 못 알아듣는 것이다.\n"

printf "\n"
monitor resume
detach
quit
