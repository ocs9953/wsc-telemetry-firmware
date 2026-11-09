$ErrorActionPreference = 'Stop'
$projectRoot = (Resolve-Path (Join-Path $PSScriptRoot '../..')).Path
$source = Get-Content -LiteralPath (Join-Path $projectRoot 'FileX/App/app_filex.c') -Raw -Encoding UTF8
$remount = [regex]::Match($source, '(?s)UINT FX_SD_Remount\(void\)\s*\{.*?\r?\n\}')
if (-not $remount.Success) { throw 'Cannot locate FX_SD_Remount' }
$outputDir = Join-Path $projectRoot 'build/host_sd_remount'
New-Item -ItemType Directory -Force -Path $outputDir | Out-Null
$executable = Join-Path $outputDir 'test_sd_remount.exe'
$before = @'
#include <assert.h>
#include <stdio.h>
#include <stdint.h>
#include <string.h>
typedef unsigned int UINT;
typedef void VOID;
#define FX_SUCCESS 0u
#define FX_IO_ERROR 1u
#define FX_NULL 0
#define FX_SD_VOLUME_NAME "test"
#define PHASE5_SD_LOGGING 1
static int sdio_disk, fx_stm32_sd_driver;
static char fx_sd_media_memory[512], fx_fault_tolerant_buffer[512];
static int sd_rx_semaphore, sd_tx_semaphore;
volatile unsigned char g_sd_hw_ok, g_init_nonfatal;
volatile unsigned char g_sd_recover_req;   /* 복구 중 가드 — FX_SD_Remount 가 내린다 */
volatile unsigned int  g_sd_timeout_run;   /* 빠른 실패 창 — 재초기화 시 0 으로 */
volatile unsigned int  g_sd_remount_reset;
/* The peripheral reset the remount does before re-init. Counted here so the
   test can assert it happens on every attempt: without it a stuck DPSMACT
   survives HAL_SD_DeInit and every later HAL_SD_Init fails. */
static int forced_resets;
#define HAL_SD_STATE_RESET 0u
static struct { unsigned int State; } hsd1 = {1u};
#define __HAL_RCC_SDMMC1_FORCE_RESET()    (++forced_resets)
#define __HAL_RCC_SDMMC1_RELEASE_RESET()  ((void)0)
static int abort_cleans, hw_fails, open_fails, hardware_stopped;
static int deletes, opens;
static UINT fx_media_abort(int *media) {
    (void)media;
    if (abort_cleans) sd_rx_semaphore = sd_tx_semaphore = 0;
    return abort_cleans ? FX_SUCCESS : FX_IO_ERROR;
}
static UINT fx_media_close(int *media) { (void)media; return FX_IO_ERROR; }
static void HAL_SD_DeInit(void *handle) { (void)handle; hardware_stopped = 1; }
static UINT tx_semaphore_delete(int *sem) {
    assert(hardware_stopped);
    ++deletes;
    if (!*sem) return FX_IO_ERROR;
    *sem = 0;
    return FX_SUCCESS;
}
static void MX_SDMMC1_SD_Init(void) {
    assert(g_init_nonfatal == 1);
    /* The peripheral must have been reset first, on every single attempt. */
    assert(forced_resets > 0);
    assert(hsd1.State == HAL_SD_STATE_RESET);
    assert(!sd_rx_semaphore && !sd_tx_semaphore);
    hardware_stopped = 0;
    if (hw_fails) g_init_nonfatal = 2;
}
static UINT fx_media_open(int *media, const char *name, int driver, VOID *info,
                          VOID *memory, unsigned long size) {
    (void)media; (void)name; (void)driver; (void)info; (void)memory; (void)size;
    ++opens;
    assert(!sd_rx_semaphore && !sd_tx_semaphore);
    sd_rx_semaphore = sd_tx_semaphore = 1;
    return open_fails ? FX_IO_ERROR : FX_SUCCESS;
}
static UINT fx_fault_tolerant_enable(int *media, VOID *buffer, unsigned long size) {
    (void)media; (void)buffer; (void)size;
    return FX_SUCCESS;
}
'@
$after = @'
int main(void) {
    /* Dead card: ABORT skips its hook; software resources must still go. */
    sd_rx_semaphore = sd_tx_semaphore = 1;
    assert(FX_SD_Remount() == FX_SUCCESS);
    assert(deletes == 2 && opens == 1 && g_sd_hw_ok == 1);

    /* Normal ABORT already deleted them: cleanup must be harmless. */
    abort_cleans = 1;
    assert(FX_SD_Remount() == FX_SUCCESS);
    assert(deletes == 4 && opens == 2);

    /* Hardware stays down, then returns on the next attempt. */
    abort_cleans = 0;
    hw_fails = 1;
    assert(FX_SD_Remount() == FX_IO_ERROR);
    assert(!sd_rx_semaphore && !sd_tx_semaphore);
    assert(deletes == 6 && opens == 2 && !g_sd_hw_ok && !g_init_nonfatal);
    hw_fails = 0;
    assert(FX_SD_Remount() == FX_SUCCESS);
    assert(deletes == 8 && opens == 3);

    /* Failed media open leaves resources; retry must clean them again. */
    open_fails = 1;
    assert(FX_SD_Remount() == FX_IO_ERROR);
    assert(sd_rx_semaphore && sd_tx_semaphore);
    open_fails = 0;
    assert(FX_SD_Remount() == FX_SUCCESS);
    assert(deletes == 12 && opens == 5 && !g_init_nonfatal);

    /* A successful re-init must close BOTH guards. Leaving the fast-fail run
       count raised makes the revived card time out on its first read (1ms
       budget), which raises the recovery request again — remount never ends. */
    g_sd_recover_req = 1;
    g_sd_timeout_run = 5;
    assert(FX_SD_Remount() == FX_SUCCESS);
    assert(g_sd_recover_req == 0 && g_sd_timeout_run == 0);

    /* Every attempt resets the peripheral, failures included. */
    { int before = forced_resets;
      assert(FX_SD_Remount() == FX_SUCCESS);
      assert(forced_resets == before + 1); }

    /* Hardware still down: the guards must stay raised, not be cleared early. */
    g_sd_recover_req = 1;
    g_sd_timeout_run = 5;
    hw_fails = 1;
    assert(FX_SD_Remount() == FX_IO_ERROR);
    assert(g_sd_recover_req == 1 && g_sd_timeout_run == 5);
    hw_fails = 0;

    puts("PASS: SD remount lifecycle (7 scenarios)");
    return 0;
}
'@
($before + "`n" + $remount.Value + "`n" + $after) |
    & gcc -x c -std=c11 -Wall -Wextra -Werror -o $executable -
if ($LASTEXITCODE -ne 0) { throw 'SD remount test compilation failed' }
& $executable
if ($LASTEXITCODE -ne 0) { throw 'SD remount lifecycle test failed' }
