const fs = require('fs'), net = require('net'), path = require('path');
const arg = (name, fallback) => process.argv.find(x => x.startsWith(name + '='))?.slice(name.length + 1) ?? fallback;
const build = arg('--build', 'build/bench');
const output = arg('--output', 'build/observation');
const seconds = Number(arg('--seconds', '45'));
const map = fs.readFileSync(path.join(build, 'WSC_Transmitter_Code_V1.map'), 'utf8');
function sym(name) {
    const m = map.match(new RegExp('0x([0-9a-f]+)\\s+' + name + '\\s*(?:\\r?\\n)', 'i'))
        || map.match(new RegExp('\\.bss\\.' + name + '\\s+0x([0-9a-f]+)\\s+', 'i'));
    if (!m) throw new Error('Missing symbol: ' + name);
    return parseInt(m[1], 16);
}
const names = ['uwTick', 'g_boot_reset_csr', 'g_sd_media_ok', 'g_sd_hw_ok',
    'g_sd_recover_req', 'g_sd_recover_ok', 'g_sd_remount_reset', 'g_sd_hard_reset',
    'g_sd_io_timeout', 'g_sd_cmd13_timeout', 'g_sd_timeout_run', 'g_sd_discarded',
    'g_sd_write_lost', 'g_uart_q_drops', 'g_sd_q_drops',
    '_tx_semaphore_created_count', '_tx_thread_created_count',
    'g_can_rx_frames', 'g_can_rx_lost', 'g_can_gap_max_ms', 'g_can_seq_frames',
    'g_can_seq_missed', 'g_can_seq_restarts', 'g_can_short_frames',
    'g_mcp_rx_frames', 'g_mcp_rx_ovf', 'g_mcp_gap_max_ms', 'g_mcp_tec', 'g_mcp_rec'];
const addresses = Object.fromEntries(names.map(n => [n, sym(n)]));
const freshAddr = sym('g_fresh_count'), gapAddr = sym('g_fresh_gap_max');
const tickAddr = sym('fresh_tick');
const countLine = map.match(/\.bss\.g_fresh_count\s+0x[0-9a-f]+\s+0x([0-9a-f]+)/i);
if (!countLine) throw new Error("g_fresh_count size not found in map");
const frameCount = parseInt(countLine[1], 16) / 4;
const lvIds = [0x6f4, 0x6f8, 0x6f9, 0x6fa, 0x6fb];
const mpptOffsets = [0, 1, 2, 5, 4, 6];
const labels = Array.from({length: frameCount}, (_, i) => {
    if (i >= 22 && i < 46) return 'MPPT_0x' + (0x310 + Math.floor((i - 22) / 6) * 16 + mpptOffsets[(i - 22) % 6]).toString(16);
    if (i >= 54 && i < 54 + lvIds.length) return 'BMSLV_0x' + lvIds[i - 54].toString(16);
    return (i >= 52 && i < 54) ? 'ADC_' + (i - 52) : 'fresh_' + i;
});
const delay = ms => new Promise(resolve => setTimeout(resolve, ms));
const socket = net.connect(4444, '127.0.0.1');
let buffer = '', pending = null;
socket.on('data', chunk => {
    buffer += chunk.toString().replace(/\0/g, '');
    if (pending && /\r?\n\r?>\s*$/.test(buffer) &&
        (pending.expected.test(buffer) || /failed|error|invalid/i.test(buffer))) {
        const p = pending; pending = null; clearTimeout(p.timer); p.resolve(buffer);
    }
});
socket.on('error', error => { if (pending) { clearTimeout(pending.timer); pending.reject(error); pending = null; } });
function command(text) {
    if (pending) throw new Error('Concurrent command');
    return new Promise((resolve, reject) => {
        buffer = '';
        const address = text.match(/0x([0-9a-f]+)/i)[1];
        pending = {resolve, reject, expected:new RegExp('0x' + address + ':\\s+[0-9a-f]', 'i'),
            timer: setTimeout(() => { pending = null; reject(new Error('Debugger timeout')); }, 3000)};
        socket.write(text + '\n');
    });
}
async function read(address, count = 1, half = false) {
    const response = await command(`${half ? 'mdh' : 'mdw'} 0x${address.toString(16)} ${count}`);
    const values = [];
    const width = half ? 4 : 8;
    for (const raw of response.split('\n')) {
        const line = raw.replace(/^\s*>\s*/, '');
        const m = line.match(/^\s*0x([0-9a-f]+):\s+([0-9a-f ]+)\s*$/i);
        if (!m) continue;
        if (parseInt(m[1], 16) !== address + values.length * (half ? 2 : 4)) throw new Error('Unexpected memory address');
        for (const word of m[2].trim().split(/\s+/)) {
            if (word.length !== width) throw new Error('Invalid memory word');
            values.push(parseInt(word, 16));
        }
    }
    if (values.length !== count) throw new Error('Incomplete memory reply: ' + response);
    return count === 1 ? values[0] : values;
}
async function snapshot() {
    const r = {utc: Date.now()};
    for (const [name, address] of Object.entries(addresses)) r[name] = await read(address);
    for (const n of ['g_sd_media_ok', 'g_sd_hw_ok', 'g_sd_recover_req']) r[n] &= 255;
    for (const n of ['UART_Queue', 'SD_Queue']) r[n] = await read(sym(n) + 16, 2);
    r.counts = await read(freshAddr, frameCount);
    r.gaps = await read(gapAddr, frameCount, true);
    r.lastTicks = await read(tickAddr, frameCount);
    r.sdClockEnabled = !!((await read(0x46020c8c)) & (1 << 27));
    r.sdIrqEnabled = !!((await read(0xe000e108)) & (1 << 14));
    return r;
}
(async () => {
    await new Promise((resolve, reject) => {socket.once('connect', resolve); socket.once('error', reject);});
    await delay(300);
    fs.mkdirSync(output, {recursive: true});
    const file = path.join(output, 'board_observation.jsonl');
    if (fs.existsSync(file)) throw new Error('Use a new output directory; evidence must not be overwritten');
    fs.writeFileSync(path.join(output, 'freshness_labels.json'), JSON.stringify(labels, null, 2));
    const start = Date.now();
    let first, last, resetSeen = false;
    do {
        const r = await snapshot();
        if (last && r.uwTick < last.uwTick) resetSeen = true;
        fs.appendFileSync(file, JSON.stringify(r) + '\n');
        if (!first) { first = r; console.log('Observation started: ' + JSON.stringify({utc:r.utc,tick:r.uwTick,media:r.g_sd_media_ok})); }
        last = r;
        if (Date.now() - start >= seconds * 1000) break;
        await delay(500);
    } while (true);
    console.log(JSON.stringify({resetSeen, elapsedBoardMs:last.uwTick-first.uwTick,
        recoveries:last.g_sd_recover_ok-first.g_sd_recover_ok,
        mediaOk:last.g_sd_media_ok, recoverReq:last.g_sd_recover_req,
        canAFrames:last.g_can_rx_frames-first.g_can_rx_frames,
        canBFrames:last.g_mcp_rx_frames-first.g_mcp_rx_frames,
        heartbeatMissed:last.g_can_seq_missed-first.g_can_seq_missed}));
})().catch(e => {console.error(e.message); process.exitCode = 1;}).finally(() => socket.destroy());
