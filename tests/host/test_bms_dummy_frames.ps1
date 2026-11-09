$ErrorActionPreference = 'Stop'
$projectRoot = (Resolve-Path (Join-Path $PSScriptRoot '../..')).Path
$receiver = Get-Content -LiteralPath (Join-Path $projectRoot 'Core/Src/can_receiver.c') -Raw -Encoding UTF8

function Cut([string]$pattern, [string]$what) {
    $m = [regex]::Match($receiver, $pattern)
    if (!$m.Success) { throw "not found in can_receiver.c: $what" }
    return $m.Value
}
$defines  = ([regex]::Matches($receiver, '(?m)^#define CAN_OFS_BMS_\w+\s+0x[0-9A-Fa-f]+u') | ForEach-Object Value) -join "`n"
$leHelp   = ([regex]::Matches($receiver, '(?m)^static inline (uint16_t|int16_t|uint32_t|int32_t) le_(u16|i16|u32|i32)\(.*\r?\n\{.*\}') | ForEach-Object Value) -join "`n"
if (([regex]::Matches($defines, '#define')).Count -lt 12) { throw 'CAN_OFS_BMS_* defines not found' }
$parts = @(
    (Cut '(?s)static float CAN_ExtractFloat\(.*?\r?\n\}' 'CAN_ExtractFloat'),
    $leHelp,
    (Cut '(?s)static inline uint16_t f_to_u16\(.*?\r?\n\}' 'f_to_u16'),
    (Cut '(?s)static inline uint16_t u32_div_sat_u16\(.*?\r?\n\}' 'u32_div_sat_u16'),
    (Cut '(?s)static inline int16_t i32_div_sat_i16\(.*?\r?\n\}' 'i32_div_sat_i16'),
    (Cut '(?s)static void CAN_StoreBmsCmu\(BmsData_t \*b, uint32_t rel,.*?\r?\n\}' 'CAN_StoreBmsCmu'),
    (Cut '(?s)static void CAN_StoreBmsPack\(BmsData_t \*b, uint32_t ofs,.*?\r?\n\}' 'CAN_StoreBmsPack')
)

$outputDir = Join-Path $projectRoot 'build/host_bms_dummy'
$stubDir = Join-Path $outputDir 'stub'
New-Item -ItemType Directory -Force -Path $stubDir | Out-Null
$executable = Join-Path $outputDir 'test_bms_dummy_frames.exe'

Set-Content -LiteralPath (Join-Path $stubDir 'main.h') -Encoding ASCII -Value @'
#pragma once
#include <stdint.h>
#include <string.h>
#define HAL_CAN_MODULE_ENABLED
typedef struct { int unused; } CAN_HandleTypeDef;
uint32_t HAL_GetTick(void);
'@
$dummyShim = Join-Path $outputDir 'dummy_shim.c'
Set-Content -LiteralPath $dummyShim -Encoding ASCII -Value @'
#include "main.h"
#include "wsc_dummy_common.h"
typedef struct { uint16_t id; uint8_t dlc; uint8_t d[8]; uint32_t tick; } Frame;
Frame g_frames[4000]; int g_nframes;
uint32_t g_tick;
uint32_t HAL_GetTick(void) { return g_tick; }
uint8_t wsc_can_send_dlc(CAN_HandleTypeDef *h, uint16_t id, const uint8_t *d, uint8_t dlc) {
    (void)h;
    if (g_nframes < 4000) { Frame *f = &g_frames[g_nframes++]; f->id = id; f->dlc = dlc; memcpy(f->d, d, 8); f->tick = g_tick; }
    return 0;
}
uint8_t wsc_can_send(CAN_HandleTypeDef *h, uint16_t id, const uint8_t *d) { return wsc_can_send_dlc(h, id, d, 8); }
void wsc_heartbeat_tick(CAN_HandleTypeDef *h, uint16_t id, uint32_t p) { (void)h; (void)id; (void)p; }
'@

$parserSource = @"
#include <assert.h>
#include <stdint.h>
#include <stdio.h>
#include <string.h>
#include "types.h"
$defines
$($parts -join "`n`n")

typedef struct { uint16_t id; uint8_t dlc; uint8_t d[8]; uint32_t tick; } Frame;
extern Frame g_frames[]; extern int g_nframes; extern uint32_t g_tick;
typedef struct { int unused; } CAN_HandleTypeDef;
void BMS_Dummy_Init(CAN_HandleTypeDef *hcan);
void BMS_Dummy_Task(void);

static int count_id(unsigned id) { int n = 0; for (int i = 0; i < g_nframes; i++) n += g_frames[i].id == id; return n; }

static void check_pack(const char *name, const BmsData_t *b, int cmus, int cells, int lo, int hi) {
    int mn = 99999, mx = -99999, mn_i = -1, mx_i = -1; long sum = 0;
    for (int n = 0; n < cmus; n++) {
        assert(b->cmu[n].serial != 0 && b->cmu[n].last_tick != 0);
        for (int k = 0; k < 8; k++) {
            int idx = n * 8 + k, v = b->cmu[n].cell_mv[k];
            if (idx >= cells) { assert(v == -32768); continue; }
            assert(v >= lo && v <= hi);
            sum += v;
            if (v < mn) { mn = v; mn_i = idx; }
            if (v > mx) { mx = v; mx_i = idx; }
        }
    }
    /* 0xF8 comes at 10 Hz and the cells at 1 Hz, so compare only when both were
       sent on the same tick - the run ends on a 1 Hz boundary, so they were. */
    assert(b->min_cell_mv == mn && b->max_cell_mv == mx);
    assert(b->min_cell_mv_cmu == mn_i / 8 && b->min_cell_mv_cell == mn_i % 8);
    assert(b->max_cell_mv_cmu == mx_i / 8 && b->max_cell_mv_cell == mx_i % 8);
    assert(b->pack_v_cv == (uint16_t)(sum / 10));
    assert(b->cmu_count == cmus);
    assert(b->soc_permille >= 550 && b->soc_permille <= 1000);
    assert(b->precharge_state == 4 && b->contactor_err_seen == 0 && b->ext_status_seen == 0);
    printf("  %s: %d CMU, cells %d..%d mV, pack %.2f V, SOC %.1f %%, min
           name, cmus, mn, mx, b->pack_v_cv / 100.0, b->soc_permille / 10.0, mn_i, mx_i);
}

int main(void) {
    static BmsData_t hv, lv;
    g_tick = 0;
    BMS_Dummy_Init(0);
    for (g_tick = 1; g_tick <= 3000; g_tick++) BMS_Dummy_Task();

    for (int i = 0; i < g_nframes; i++) {
        const Frame *f = &g_frames[i];
        assert(f->dlc == 8);
        BmsData_t *b; unsigned base; int cmus;
        if (f->id >= 0x040 && f->id <= 0x13F)      { b = &hv; base = 0x040; cmus = BMS_CMU_COUNT; }
        else if (f->id >= 0x600 && f->id <= 0x6FF) { b = &lv; base = 0x600; cmus = BMS_LV_CMU_COUNT; }
        else { printf("frame outside both BMU blocks: 0x%03X\n", f->id); return 1; }
        unsigned ofs = f->id - base;
        if ((ofs >= 0xF0 && ofs <= 0xF3) || ofs >= 0xFE) { printf("reserved offset sent: 0x%03X\n", f->id); return 1; }
        /* same dispatch as can_receiver.c */
        if (ofs >= CAN_OFS_BMS_CMU_FIRST && ofs <= CAN_OFS_BMS_CMU_LAST)
            CAN_StoreBmsCmu(b, ofs - CAN_OFS_BMS_CMU_FIRST, f->d, f->tick, (uint32_t)cmus);
        else
            CAN_StoreBmsPack(b, ofs, f->d, f->tick);
    }

    const unsigned bases[2] = {0x040, 0x600};
    const unsigned fast[] = {0xF6, 0xF8, 0xFA};
    const unsigned slow[] = {0x00, 0xF4, 0xF5, 0xF7, 0xF9, 0xFB, 0xFC, 0xFD};
    for (int p = 0; p < 2; p++) {
        for (unsigned i = 0; i < 3; i++) assert(count_id(bases[p] + fast[i]) == 30);
        for (unsigned i = 0; i < 8; i++) assert(count_id(bases[p] + slow[i]) == 3);
    }
    for (unsigned o = 0x01; o <= 0x0C; o++) assert(count_id(0x040 + o) == 3);
    for (unsigned o = 0x01; o <= 0x03; o++) assert(count_id(0x600 + o) == 3);
    assert(count_id(0x604) == 0);

    printf("%d frames in 3 s (%.1f/s)\n", g_nframes, g_nframes / 3.0);
    check_pack("HV", &hv, 4, 32, 3000, 4150);
    check_pack("LV", &lv, 1, 4, 3000, 3500);
    puts("BMS dummy frames parse as the receiver expects");
    return 0;
}
"@

$parserFile = Join-Path $outputDir 'parser_side.c'
Set-Content -LiteralPath $parserFile -Encoding UTF8 -Value $parserSource
$dummyDir = Join-Path $projectRoot 'tests/dummy_nodes'
# The stub directory goes first: Core/Inc has the firmware's own main.h (U5 HAL),
# which the dummy must not pick up. Core/Inc is only there for types.h.
& gcc -std=c11 -Wall -Wextra -Werror -Wno-unused-function `
    -I $stubDir -I (Join-Path $dummyDir 'common') -I (Join-Path $dummyDir 'BMS_Dummy/App') -I (Join-Path $projectRoot 'Core/Inc') `
    -o $executable $parserFile $dummyShim (Join-Path $dummyDir 'BMS_Dummy/App/bms_dummy.c')
if ($LASTEXITCODE -ne 0) { throw 'host build failed' }
& $executable
if ($LASTEXITCODE -ne 0) { throw 'BMS dummy frame test failed' }
