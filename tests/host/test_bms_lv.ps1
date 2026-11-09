$ErrorActionPreference = 'Stop'
$projectRoot = (Resolve-Path (Join-Path $PSScriptRoot '../..')).Path
$source = Get-Content -LiteralPath (Join-Path $projectRoot 'Core/Src/can_receiver.c') -Raw -Encoding UTF8
$helpers = [regex]::Matches($source, '(?m)^static inline (uint16_t|int16_t|uint32_t) le_(u16|i16|u32)\(.*\r?\n\{.*\}')
$body = [regex]::Match($source, '(?s)static void CAN_StoreBmsCmu\(BmsData_t \*b, uint32_t rel,.*?\r?\n\}')
if ($helpers.Count -lt 3 -or !$body.Success) { throw 'Production CMU parser or byte helpers not found' }

$outputDir = Join-Path $projectRoot 'build/host_bms_lv'
New-Item -ItemType Directory -Force -Path $outputDir | Out-Null
$executable = Join-Path $outputDir 'test_bms_lv.exe'

$before = @'
#include <assert.h>
#include <stdint.h>
#include <stdio.h>
#include <string.h>
#define BMS_CELLS_PER_CMU 8
#define BMS_CMU_COUNT     4
#define BMS_LV_CMU_COUNT  1
typedef struct {
  uint32_t serial; int16_t pcb_temp; int16_t cell_temp;
  int16_t  cell_mv[BMS_CELLS_PER_CMU]; uint32_t last_tick;
} BmsCmu_t;
typedef struct { BmsCmu_t cmu[BMS_CMU_COUNT]; } BmsData_t;
'@

$after = @'
static void frame_cells(uint8_t *d, int16_t a, int16_t b, int16_t c, int16_t e) {
    int16_t v[4] = {a, b, c, e};
    for (int k = 0; k < 4; k++) { d[k*2] = (uint8_t)((uint16_t)v[k] & 0xFF); d[k*2+1] = (uint8_t)((uint16_t)v[k] >> 8); }
}
int main(void) {
    BmsData_t lv, hv;
    uint8_t d[8];
    memset(&lv, 0x5A, sizeof lv);          /* sentinel: anything untouched stays 0x5A5A */
    memset(&hv, 0, sizeof hv);

    /* LV CMU0: status, cells 0..3 real, cells 4..7 absent */
    memset(d, 0, 8); d[0] = 0x01; d[1] = 0x00; d[2] = 0x56; d[3] = 0x4C;   /* serial 0x4C560001 */
    d[4] = 0x2C; d[5] = 0x01; d[6] = 0x18; d[7] = 0x01;                   /* 30.0C, 28.0C */
    CAN_StoreBmsCmu(&lv, 0, d, 100, BMS_LV_CMU_COUNT);
    frame_cells(d, 3301, 3302, 3303, 3304);
    CAN_StoreBmsCmu(&lv, 1, d, 101, BMS_LV_CMU_COUNT);
    frame_cells(d, -32768, -32768, -32768, -32768);
    CAN_StoreBmsCmu(&lv, 2, d, 102, BMS_LV_CMU_COUNT);

    assert(lv.cmu[0].serial == 0x4C560001u);
    assert(lv.cmu[0].pcb_temp == 300 && lv.cmu[0].cell_temp == 280);
    assert(lv.cmu[0].cell_mv[0] == 3301 && lv.cmu[0].cell_mv[3] == 3304);
    for (int k = 4; k < 8; k++) assert(lv.cmu[0].cell_mv[k] == -32768);
    assert(lv.cmu[0].last_tick == 102);

    /* Frames for CMU 1..3 on the LV bus are dropped: the LV pack has one CMU. */
    frame_cells(d, 1111, 2222, 3333, 4444);
    for (uint32_t rel = 3; rel < 12; rel++) CAN_StoreBmsCmu(&lv, rel, d, 999, BMS_LV_CMU_COUNT);
    for (int n = 1; n < BMS_CMU_COUNT; n++) {
        assert((uint16_t)lv.cmu[n].cell_mv[0] == 0x5A5A);   /* never written */
        assert(lv.cmu[n].last_tick == 0x5A5A5A5Au);
    }

    /* HV with its own count still reaches all four CMUs. */
    for (uint32_t n = 0; n < BMS_CMU_COUNT; n++) {
        frame_cells(d, (int16_t)(3500 + n), 0, 0, 0);
        CAN_StoreBmsCmu(&hv, 3 * n + 1, d, 200 + n, BMS_CMU_COUNT);
        assert(hv.cmu[n].cell_mv[0] == (int16_t)(3500 + n));
    }
    /* ...and not a fifth. */
    frame_cells(d, 7777, 0, 0, 0);
    CAN_StoreBmsCmu(&hv, 3 * 4 + 1, d, 300, BMS_CMU_COUNT);
    for (uint32_t n = 0; n < BMS_CMU_COUNT; n++) assert(hv.cmu[n].cell_mv[0] != 7777);

    puts("PASS: LV BMS CMU parsing (bound, 4S no-cell markers, HV unaffected)");
    return 0;
}
'@

$helperText = ($helpers | ForEach-Object { $_.Value }) -join "`n"
($before + "`n" + $helperText + "`n" + $body.Value + "`n" + $after) |
    & gcc -x c -std=c11 -Wall -Wextra -Werror -o $executable -
if ($LASTEXITCODE -ne 0) { throw 'LV BMS test compilation failed' }
& $executable
if ($LASTEXITCODE -ne 0) { throw 'LV BMS CMU parsing failed' }
