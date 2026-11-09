$ErrorActionPreference = 'Stop'
$projectRoot = (Resolve-Path (Join-Path $PSScriptRoot '../..')).Path
$source = Get-Content -LiteralPath (Join-Path $projectRoot 'FileX/Target/fx_stm32_sd_driver_glue.c') -Raw -Encoding UTF8
$body = [regex]::Match($source, '(?s)INT fx_stm32_sd_get_status\(UINT instance\)\s*\{.*?\r?\n\}')
$wedged = [regex]::Match($source, '(?m)^#define FX_SD_WEDGED_TICKS\s+\(.*\)')
$window = [regex]::Match($source, '(?s)ULONG fx_sd_status_wait_ticks\(void\)\s*\{.*?\r?\n\}')
if (!$body.Success -or !$wedged.Success -or !$window.Success) { throw 'Production get_status function not found' }

$outputDir = Join-Path $projectRoot 'build/host_sd_get_status'
New-Item -ItemType Directory -Force -Path $outputDir | Out-Null
$executable = Join-Path $outputDir 'test_sd_get_status.exe'

$before = @'
#include <assert.h>
#include <stdint.h>
#include <stdio.h>
#define SDMMC_STA_DPSMACT 0x1000u
#define SDMMC_STA_CPSMACT 0x2000u
#define HAL_SD_STATE_RESET 0u
#define HAL_SD_STATE_READY 1u
#define HAL_SD_CARD_TRANSFER 4u
#define TX_TIMER_TICKS_PER_SECOND 1000u
typedef int INT;
typedef unsigned int UINT;
typedef unsigned long ULONG;
#define UNUSED(x) ((void)(x))
typedef struct { volatile uint32_t STA; } Registers;
typedef struct { Registers *Instance; uint32_t State; } Handle;
static Registers regs;
static Handle hsd1 = { &regs, HAL_SD_STATE_READY };
static volatile uint8_t g_sd_recover_req;
static uint32_t fake_now;
static unsigned hal_calls;
static uint32_t card_state = HAL_SD_CARD_TRANSFER;
static ULONG tx_time_get(void) { return fake_now; }
static uint32_t HAL_SD_GetCardState(Handle *h) {
    (void)h;
    ++hal_calls;
    /* Standing in for the real thing: on a wedged peripheral this does not
       return for seconds. Production no longer reaches it at all, and this
       stub is kept so a regression that restores the call is caught. */
    return card_state;
}
/* The bounded CMD13 check that replaced it. Counted separately: while the
   state machine runs, the guard must answer without even sending a command. */
static unsigned bounded_calls;
static INT bounded_result;
static INT fx_sd_card_ready_bounded(void) { ++bounded_calls; return bounded_result; }
'@

$after = @'
int main(void) {
    /* Idle state machine: asking the card is the right thing to do. */
    regs.STA = 0;
    bounded_result = 0;
    assert(fx_stm32_sd_get_status(0) == 0);
    assert(bounded_calls == 1 && hal_calls == 0);

    /* Data path active: answer from the register, do not enter the HAL —
       not on the first call either, which is when a wedge is first seen. */
    bounded_calls = 0;
    fake_now = 1000;
    regs.STA = SDMMC_STA_DPSMACT;
    assert(fx_stm32_sd_get_status(0) == 1);
    assert(bounded_calls == 0 && hal_calls == 0);
    assert(g_sd_recover_req == 0);   /* too early to call it wedged */

    /* Still active but under the limit: still no HAL, still no escalation. */
    fake_now = 1000 + (TX_TIMER_TICKS_PER_SECOND / 4u);
    assert(fx_stm32_sd_get_status(0) == 1);
    assert(bounded_calls == 0 && g_sd_recover_req == 0);

    /* Past the limit: escalate to recovery, still without entering the HAL. */
    fake_now = 1001 + (TX_TIMER_TICKS_PER_SECOND / 4u);
    assert(fx_stm32_sd_get_status(0) == 1);
    assert(bounded_calls == 0 && hal_calls == 0);
    assert(g_sd_recover_req == 1);

    /* Recovery pending: short-circuit regardless of the register. */
    regs.STA = 0;
    assert(fx_stm32_sd_get_status(0) == 1);
    assert(bounded_calls == 0 && hal_calls == 0);

    /* Handle torn down for re-init: same, no HAL call on a dead handle. */
    g_sd_recover_req = 0;
    hsd1.State = HAL_SD_STATE_RESET;
    assert(fx_stm32_sd_get_status(0) == 1);
    assert(bounded_calls == 0 && hal_calls == 0);

    /* Command path alone counts as active too. */
    hsd1.State = HAL_SD_STATE_READY;
    regs.STA = SDMMC_STA_CPSMACT;
    fake_now = 50000;
    assert(fx_stm32_sd_get_status(0) == 1);
    assert(hal_calls == 0);
    fake_now = 50001 + (TX_TIMER_TICKS_PER_SECOND / 4u);
    assert(fx_stm32_sd_get_status(0) == 1);
    assert(g_sd_recover_req == 1 && bounded_calls == 0 && hal_calls == 0);

    /* Back to idle after recovery cleared the flag: the HAL is used again,
       and a card that is not in TRANSFER is reported busy. */
    g_sd_recover_req = 0;
    regs.STA = 0;
    bounded_calls = 0;
    bounded_result = 1;
    assert(fx_stm32_sd_get_status(0) == 1);
    assert(bounded_calls == 1);
    bounded_result = 0;
    assert(fx_stm32_sd_get_status(0) == 0);
    assert(bounded_calls == 2);
    /* The unbounded HAL call must stay unreached on every path above. */
    assert(hal_calls == 0);

    /* The retry window check_sd_status busies itself with. A healthy card
       gets the full 750ms; once recovery is needed the window collapses, or
       the caller burns 750ms per driver request and a single file write adds
       those up past the 4s watchdog. */
    g_sd_recover_req = 0;
    hsd1.State = HAL_SD_STATE_READY;
    assert(fx_sd_status_wait_ticks() == 750);
    g_sd_recover_req = 1;
    assert(fx_sd_status_wait_ticks() == 1);
    g_sd_recover_req = 0;
    hsd1.State = HAL_SD_STATE_RESET;
    assert(fx_sd_status_wait_ticks() == 1);

    puts("PASS: get_status guard, bounded check, and the collapsing retry window");
    return 0;
}
'@

($before + "`n" + $wedged.Value + "`n" + $window.Value + "`n" + $body.Value + "`n" + $after) |
    & gcc -x c -std=c11 -Wall -Wextra -Werror -o $executable -
if ($LASTEXITCODE -ne 0) { throw 'SD get_status test compilation failed' }
& $executable
if ($LASTEXITCODE -ne 0) { throw 'SD get_status guard test failed' }
