const fs = require('fs'), path = require('path');
const opt = (name, dflt) => {
    const a = process.argv.find(x => x.startsWith('--' + name + '='));
    return a ? Number(a.slice(name.length + 3)) : dflt;
};
const positional = process.argv.slice(2).filter(x => !x.startsWith('--'));
const dir = positional[0] || 'build/reset_watch';
const EXPECT_S = opt('expect-seconds', 0);
const MAX_GAP_MS = opt('max-gap-ms', 5000);

const file = path.join(dir, 'radio_chunks.jsonl');
if (!fs.existsSync(file)) { console.error('no capture at ' + file); process.exit(2); }

const SRC_SYS = 0, KEY_BOOT_REPORT = 0x95, KEY_WATCHDOG = 0x91, KEY_STACK_OVERFLOW = 0x94;
const CSR_FLAGS = [[29, 'IWDG'], [30, 'WWDG'], [28, 'SOFT'], [26, 'PIN'], [27, 'BOR'], [31, 'LPWR'], [25, 'OBL']];
const decodeReset = v => CSR_FLAGS.filter(([b]) => v & (1 << b)).map(([, n]) => n).join('+') || 'none';

function cobsDecode(buf) {
    const out = [];
    for (let i = 0; i < buf.length;) {
        const n = buf[i++];
        if (!n || i + n - 1 > buf.length) return null;
        for (let j = 1; j < n; j++) out.push(buf[i++]);
        if (n !== 255 && i < buf.length) out.push(0);
    }
    return Buffer.from(out);
}
function crc8(data) {
    let crc = 0;
    for (const b of data) {
        crc ^= b;
        for (let i = 0; i < 8; i++) crc = (crc & 0x80) ? ((crc << 1) ^ 0x07) & 0xff : (crc << 1) & 0xff;
    }
    return crc;
}

const packets = [];
let frame = [], discardFirst = true, cobsErrors = 0, crcErrors = 0, otherFrames = 0;
for (const line of fs.readFileSync(file, 'utf8').split('\n')) {
    if (!line.trim()) continue;
    const chunk = JSON.parse(line);
    for (const byte of Buffer.from(chunk.data, 'base64')) {
        if (byte !== 0) { frame.push(byte); continue; }
        if (discardFirst) { frame = []; discardFirst = false; continue; }
        if (!frame.length) continue;
        const data = cobsDecode(Buffer.from(frame));
        frame = [];
        if (!data) { cobsErrors++; continue; }
        if (data.length !== 17 || data[0] !== 1) { otherFrames++; continue; }
        const log = data.subarray(1);
        if (crc8(log.subarray(0, 15)) !== log[15]) { crcErrors++; continue; }
        packets.push({utc: chunk.utc, elapsedMs: chunk.elapsedMs, ts: log.readUInt32LE(0),
                      level: log[4], src: log[5], key: log[6], value: log.subarray(7, 15)});
    }
}
if (!packets.length) { console.error('no valid packets decoded - INCONCLUSIVE, nothing was heard'); process.exit(2); }

const SETTLE_MS = Number(positional[1] || 5000);
const captureStart = packets[0].utc;

const events = [];
let previousTs = null, previousUtc = null, maxGapMs = 0, gapAt = null;
const silences = [];
for (const p of packets) {
    const settling = (p.utc - captureStart) < SETTLE_MS;
    if (previousTs !== null) {
        if (p.ts < previousTs) {
            events.push({kind: 'restart (board clock went backwards)', utc: p.utc, settling,
                         detail: 'timestamp ' + previousTs + ' -> ' + p.ts});
        }
        const gap = p.utc - previousUtc;
        if (gap > maxGapMs) { maxGapMs = gap; gapAt = p.utc; }
        if (gap > MAX_GAP_MS) silences.push({from: previousUtc, ms: gap});
    }
    previousTs = p.ts; previousUtc = p.utc;

    if (p.src === SRC_SYS && p.key === KEY_BOOT_REPORT) {
        const csr = p.value.readUInt32LE(0);
        events.push({kind: 'boot report', utc: p.utc, settling,
                     detail: 'cause ' + decodeReset(csr) + ' (RCC_CSR 0x' + csr.toString(16) + '), board up ' + p.ts + 'ms'});
    }
    if (p.src === SRC_SYS && p.key === KEY_WATCHDOG) {
        events.push({kind: 'watchdog reset reported', utc: p.utc, settling,
                     detail: 'RCC_CSR 0x' + p.value.readUInt32LE(0).toString(16)});
    }
    if (p.src === SRC_SYS && p.key === KEY_STACK_OVERFLOW) {
        events.push({kind: 'stack overflow reported', utc: p.utc, settling,
                     detail: 'thread ' + p.value.readUInt32LE(0)});
    }
}

const span = (packets[packets.length - 1].utc - packets[0].utc) / 1000;
console.log('capture   : ' + dir);
console.log('packets   : ' + packets.length + ' valid over ' + span.toFixed(1) + 's');
console.log('errors    : ' + cobsErrors + ' framing, ' + crcErrors + ' CRC, ' + otherFrames + ' other');
console.log('max gap   : ' + maxGapMs + 'ms' + (gapAt ? ' at ' + new Date(gapAt).toISOString() : ''));
console.log('');
const stale = events.filter(e => e.settling);
const real = events.filter(e => !e.settling);

if (stale.length) {
    console.log('Capture-start transients (first ' + (SETTLE_MS / 1000) + 's, radio buffer draining - not findings):');
    for (const e of stale) console.log('  ' + new Date(e.utc).toISOString() + '  ' + e.kind + ' - ' + e.detail);
    console.log('');
}
const firstS = packets[0].elapsedMs / 1000;
const heardS = packets[packets.length - 1].elapsedMs / 1000;
const shortfalls = [];
if (!EXPECT_S) shortfalls.push('no --expect-seconds given, so coverage cannot be judged');
if (packets[0].elapsedMs > MAX_GAP_MS)
    shortfalls.push('nothing heard for the first ' + firstS.toFixed(1) + 's after the port opened');
for (const g of silences)
    shortfalls.push('silent for ' + (g.ms / 1000).toFixed(1) + 's from ' + new Date(g.from).toISOString());
if (EXPECT_S && EXPECT_S * 1000 - packets[packets.length - 1].elapsedMs > MAX_GAP_MS)
    shortfalls.push('nothing heard after ' + heardS.toFixed(1) + 's of the expected ' + EXPECT_S + 's');
if (shortfalls.length > 1 || (shortfalls.length && EXPECT_S))
    shortfalls.push('(silence limit ' + (MAX_GAP_MS / 1000) + 's, --max-gap-ms)');

if (real.length) {
    console.log('VERDICT: the board restarted during the capture.');
    for (const e of real) console.log('  ' + new Date(e.utc).toISOString() + '  ' + e.kind + ' - ' + e.detail);
    console.log('');
    console.log('A boot report whose cause is PIN, with no reset command sent and nobody');
    console.log('touching the board, is the fault we are chasing. Seeing it with the');
    console.log('debugger unplugged rules the SWD path out; not seeing it there points at it.');
    if (shortfalls.length) {
        console.log('');
        console.log('Coverage was also incomplete, so more restarts may have been missed:');
        for (const f of shortfalls) console.log('  - ' + f);
    }
    process.exitCode = 1;
} else if (shortfalls.length) {
    console.log('VERDICT: INCONCLUSIVE - no restart was seen, but the board was not heard');
    console.log('for the whole run, so a restart could have gone unseen:');
    for (const f of shortfalls) console.log('  - ' + f);
    process.exitCode = 3;
} else {
    console.log('VERDICT: no restart after the settle window, and the board was heard from');
    console.log(firstS.toFixed(1) + 's to ' + heardS.toFixed(1) + 's of the expected ' + EXPECT_S + 's with no silence over ' + (MAX_GAP_MS / 1000) + 's.');
    process.exitCode = 0;
}
