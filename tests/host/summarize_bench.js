const fs = require('fs'), path = require('path');
const root = process.argv[2];
if (!root) throw new Error('Usage: node summarize_bench.js <run directory>');
const read = file => fs.readFileSync(file, 'utf8').trim().split(/\r?\n/).filter(Boolean).map(JSON.parse);
const snapshots = read(path.join(root, 'board_observation.jsonl'));
const labels = JSON.parse(fs.readFileSync(path.join(root, 'freshness_labels.json')));
const first = snapshots[0], last = snapshots.at(-1);
let resetSeen = false;
for (let i = 1; i < snapshots.length; i++) if (snapshots[i].uwTick < snapshots[i-1].uwTick) resetSeen = true;
const delta = name => last[name] - first[name];
const counters = ['g_sd_recover_ok', 'g_sd_remount_reset', 'g_sd_hard_reset',
    'g_sd_io_timeout', 'g_sd_cmd13_timeout', 'g_sd_discarded', 'g_sd_write_lost',
    'g_uart_q_drops', 'g_sd_q_drops', 'g_can_rx_frames', 'g_can_rx_lost',
    'g_can_seq_frames', 'g_can_seq_missed', 'g_can_seq_restarts',
    'g_mcp_rx_frames', 'g_mcp_rx_ovf'];
const channels = labels.map((label,i) => ({label,
    countDelta:last.counts[i]-first.counts[i],
    lifetimeGapBefore:first.gaps[i], lifetimeGapAfter:last.gaps[i],
    maxSampledAgeMs:Math.max(...snapshots.map(s=>s.lastTicks[i] ? Math.max(0,s.uwTick-s.lastTicks[i]):0)),
    everSeen:last.lastTicks[i]!==0}));
const result = {startUtc:first.utc,endUtc:last.utc,elapsedBoardMs:last.uwTick-first.uwTick,
    resetSeen,deltas:Object.fromEntries(counters.map(n=>[n,delta(n)])),
    final:{media:last.g_sd_media_ok,hw:last.g_sd_hw_ok,req:last.g_sd_recover_req,
        timeoutRun:last.g_sd_timeout_run,semaphores:last._tx_semaphore_created_count,
        threads:last._tx_thread_created_count,uartQueue:last.UART_Queue,sdQueue:last.SD_Queue,
        clockEnabled:last.sdClockEnabled,irqEnabled:last.sdIrqEnabled},
    canGapBefore:[first.g_can_gap_max_ms,first.g_mcp_gap_max_ms],
    canGapAfter:[last.g_can_gap_max_ms,last.g_mcp_gap_max_ms],channels};
const radioFile = path.join(root,'radio_analysis.json');
if (fs.existsSync(radioFile)) {
    const r=JSON.parse(fs.readFileSync(radioFile,'utf8'));
    const period=r.faultWindow||r.steadyCapture;
    result.radio={period:r.faultWindow?'fault through recovery result':'after first five seconds',
        rawFramingErrors:r.framingErrors,rawCrcErrors:r.crcErrors,
        periodErrors:r.faultErrors||r.steadyErrors,count:period.count,
        maxArrivalGap:period.maxArrivalGap,maxBoardGap:period.maxBoardGap,
        groups:period.groups};
}
fs.writeFileSync(path.join(root,'summary.json'),JSON.stringify(result,null,2));
console.log(JSON.stringify({...result,channels:channels.filter(c=>c.countDelta>0),
    radio:result.radio?{...result.radio,groups:undefined}:undefined},null,2));
