$ErrorActionPreference = 'Stop'
$projectRoot = (Resolve-Path (Join-Path $PSScriptRoot '../..')).Path
$source = Get-Content -LiteralPath (Join-Path $projectRoot 'Core/Src/can_receiver.c') -Raw -Encoding UTF8
$body = [regex]::Match($source, '(?s)static void CAN_TrackHeartbeat\(const uint8_t \*d\)\s*\{.*?\r?\n\}')
if (!$body.Success) { throw 'Production heartbeat tracker not found' }

$outputDir = Join-Path $projectRoot 'build/host_can_heartbeat'
New-Item -ItemType Directory -Force -Path $outputDir | Out-Null
$executable = Join-Path $outputDir 'test_can_heartbeat.exe'

$before = @'
#include <assert.h>
#include <stdint.h>
#include <stdio.h>
static volatile uint32_t g_can_seq_last, g_can_seq_missed, g_can_seq_frames;
static volatile uint32_t g_can_seq_restarts;
#define CAN_SEQ_RESTART_STEP 0x10000u
'@

$after = @'
static void feed(uint32_t seq) {
    uint8_t d[8] = {0};
    d[0] = (uint8_t)seq;         d[1] = (uint8_t)(seq >> 8);
    d[2] = (uint8_t)(seq >> 16); d[3] = (uint8_t)(seq >> 24);
    CAN_TrackHeartbeat(d);
}
static void reset(void) {
    g_can_seq_last = g_can_seq_missed = g_can_seq_frames = g_can_seq_restarts = 0;
}

int main(void) {
    /* First frame sets the baseline only — the sender's counter may start
       anywhere, and counting it as loss would report a bogus outage. */
    reset();
    feed(5000);
    assert(g_can_seq_missed == 0 && g_can_seq_frames == 1 && g_can_seq_last == 5000);

    /* Consecutive frames lose nothing. */
    feed(5001); feed(5002); feed(5003);
    assert(g_can_seq_missed == 0 && g_can_seq_frames == 4);

    /* A gap counts exactly the frames that went missing, not the step. */
    feed(5007);
    assert(g_can_seq_missed == 3);
    feed(5008);
    assert(g_can_seq_missed == 3);

    /* Several outages accumulate. */
    feed(5010);
    assert(g_can_seq_missed == 4);

    /* The 32-bit wrap is an ordinary +1, not a four-billion loss. */
    reset();
    feed(0xFFFFFFFEu);
    feed(0xFFFFFFFFu);
    feed(0x00000000u);
    feed(0x00000001u);
    assert(g_can_seq_missed == 0 && g_can_seq_frames == 4);

    /* A gap spanning the wrap is still counted correctly. */
    reset();
    feed(0xFFFFFFFDu);
    feed(0x00000002u);
    assert(g_can_seq_missed == 4);

    /* A restarted sender rewinds. Unsigned subtraction makes that an enormous
       step, which must not be reported as loss — the run is over at that point
       and the operator needs to see the restart, not a fabricated number. */
    reset();
    feed(9000); feed(9001);
    feed(0);
    assert(g_can_seq_missed == 0);      /* not a loss */
    assert(g_can_seq_restarts == 1);    /* reported as what it is */
    feed(1); feed(2);
    assert(g_can_seq_missed == 0 && g_can_seq_restarts == 1);

    /* A repeated sequence number (a duplicate on the bus) adds nothing. */
    reset();
    feed(10); feed(10);
    assert(g_can_seq_missed == 0);

    puts("PASS: heartbeat sequence accounting (9 cases)");
    return 0;
}
'@

($before + "`n" + $body.Value + "`n" + $after) |
    & gcc -x c -std=c11 -Wall -Wextra -Werror -o $executable -
if ($LASTEXITCODE -ne 0) { throw 'Heartbeat test compilation failed' }
& $executable
if ($LASTEXITCODE -ne 0) { throw 'Heartbeat sequence accounting failed' }
