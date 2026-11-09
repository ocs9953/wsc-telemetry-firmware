$ErrorActionPreference = 'Stop'
$projectRoot = (Resolve-Path (Join-Path $PSScriptRoot '../..')).Path
$source = Get-Content -LiteralPath (Join-Path $projectRoot 'FileX/Target/fx_stm32_sd_driver_glue.c') -Raw -Encoding UTF8
$body = [regex]::Match($source, '(?s)void fx_sd_io_timeout\(void\)\s*\{.*?\r?\n\}')
$limit = [regex]::Match($source, '(?m)^#define FX_SD_ABORT_SPIN_LIMIT\s+\d+u')
if (!$body.Success -or !$limit.Success) { throw 'Production timeout function not found' }
$outputDir = Join-Path $projectRoot 'build/host_sd_timeout'
New-Item -ItemType Directory -Force -Path $outputDir | Out-Null
$executable = Join-Path $outputDir 'test_sd_timeout.exe'
$before = @'
#include <assert.h>
#include <stdint.h>
#include <stdio.h>
#include <string.h>
#define SDMMC_IT_DATAEND 0x100u
#define SDMMC_IT_DCRCFAIL 2u
#define SDMMC_IT_DTIMEOUT 8u
#define SDMMC_IT_TXUNDERR 0x10u
#define SDMMC_IT_RXOVERR 0x20u
#define SDMMC_DISABLE_IDMA 0u
#define SDMMC_DCTRL_DTEN 1u
#define SDMMC_STA_DPSMACT 0x1000u
#define SDMMC_STA_CPSMACT 0x2000u
#define SDMMC_STATIC_DATA_FLAGS 0x53Au
#define HAL_SD_STATE_RESET 0u
#define HAL_SD_STATE_READY 1u
#define HAL_SD_ERROR_TIMEOUT 0x80000000u
#define SD_CONTEXT_NONE 0u
#define TX_NO_WAIT 0u
#define TX_SUCCESS 0u
typedef struct { volatile uint32_t IDMACTRL,CMD,DCTRL,STA,MASK,ICR; } Registers;
volatile uint32_t g_sd_timeout_run;   /* 연속 타임아웃 — 빠른 실패 판단용 */
typedef struct { Registers *Instance; uint32_t ErrorCode,Context,State; } Handle;
static Registers regs;
static Handle hsd1={&regs,0,0,0};
static unsigned sd_tx_semaphore,sd_rx_semaphore,resets,releases;
static uint32_t g_sd_hard_reset,g_sd_io_timeout;
static uint8_t g_sd_recover_req;
#define __HAL_SD_DISABLE_IT(h,mask) ((h)->Instance->MASK &= ~(mask))
#define __SDMMC_CMDTRANS_DISABLE(r) ((r)->CMD &= ~0x40u)
#define __HAL_SD_CLEAR_FLAG(h,flags) ((h)->Instance->ICR = (flags))
static void reset_peripheral(void) {
    assert(regs.IDMACTRL==0 && !(regs.CMD&0x40) && !(regs.DCTRL&1));
    ++resets; memset(&regs,0,sizeof regs);
}
#define __HAL_RCC_SDMMC1_FORCE_RESET() reset_peripheral()
#define __HAL_RCC_SDMMC1_RELEASE_RESET() (++releases)
static unsigned tx_semaphore_get(unsigned *sem,unsigned wait) {
    assert(wait==TX_NO_WAIT);
    if(!*sem) return 1u;
    --*sem; return TX_SUCCESS;
}
'@
$after = @'
static void run(uint32_t sta,unsigned initialState,unsigned expectReset) {
    memset(&regs,0,sizeof regs);
    regs.IDMACTRL=1;regs.CMD=0x40;regs.DCTRL=1;regs.STA=sta;regs.MASK=0xFFFFFFFFu;
    hsd1.State=initialState;hsd1.Context=123;hsd1.ErrorCode=0;
    sd_tx_semaphore=3;sd_rx_semaphore=2;
    g_sd_recover_req=0;
    const unsigned previousReset=g_sd_hard_reset,previousTimeout=g_sd_io_timeout;
    fx_sd_io_timeout();
    assert(!regs.IDMACTRL && !(regs.CMD&0x40) && !(regs.DCTRL&1));
    assert(!(regs.MASK&(SDMMC_IT_DATAEND|SDMMC_IT_DCRCFAIL|SDMMC_IT_DTIMEOUT|SDMMC_IT_TXUNDERR|SDMMC_IT_RXOVERR)));
    assert(!sd_tx_semaphore && !sd_rx_semaphore);
    assert(g_sd_recover_req==1 && g_sd_io_timeout==previousTimeout+1);
    assert(g_sd_hard_reset==previousReset+expectReset && resets==releases);
    assert(hsd1.ErrorCode==HAL_SD_ERROR_TIMEOUT && hsd1.Context==SD_CONTEXT_NONE);
    assert(hsd1.State==((expectReset||initialState==HAL_SD_STATE_RESET)?HAL_SD_STATE_RESET:HAL_SD_STATE_READY));
}
int main(void) {
    run(0,2,0);
    run(SDMMC_STA_DPSMACT,2,1);
    run(SDMMC_STA_CPSMACT,2,1);
    run(SDMMC_STA_DPSMACT|SDMMC_STA_CPSMACT,2,1);
    run(0,HAL_SD_STATE_RESET,0);
    assert(g_sd_io_timeout==5 && g_sd_hard_reset==3);
    puts("PASS: production SD timeout cleanup, 5 simulated register scenarios");
    return 0;
}
'@
($before + "`n" + $limit.Value + "`n" + $body.Value + "`n" + $after) |
    & gcc -x c -std=c11 -Wall -Wextra -Werror -o $executable -
if ($LASTEXITCODE -ne 0) { throw 'SD timeout test compilation failed' }
& $executable
if ($LASTEXITCODE -ne 0) { throw 'SD timeout cleanup test failed' }
