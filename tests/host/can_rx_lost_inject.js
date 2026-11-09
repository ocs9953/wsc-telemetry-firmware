const fs = require('fs'), net = require('net'), path = require('path');
const buildDir = process.argv[2] || 'build/bench';
const maskMs = Number(process.argv[3] || 300);
const map = fs.readFileSync(path.join(buildDir, 'WSC_Transmitter_Code_V1.map'), 'utf8');
function sym(n) {
    const m = map.match(new RegExp('0x([0-9a-f]+)\\s+' + n + '\\s*(?:\\r?\\n)', 'i'));
    if (!m) throw new Error('missing ' + n);
    return parseInt(m[1], 16);
}
const ISER1 = 0xe000e104, ICER1 = 0xe000e184;
const BIT = (1 << (39 - 32)) >>> 0;

const s = net.connect(4444, '127.0.0.1');
let buf = '', pending = null, restoreNeeded = false;
s.on('data', c => {
    buf += c.toString().replace(/\0/g, '');
    if (pending && /\r?\n\r?>\s*$/.test(buf)) { const p = pending; pending = null; clearTimeout(p.t); p.res(buf); }
});
const cmd = t => new Promise((res, rej) => {
    buf = '';
    pending = {res, rej, t: setTimeout(() => {pending = null; rej(new Error('timeout'));}, 3000)};
    s.write(t + '\n');
});
async function rd(a) {
    const t = await cmd('mdw 0x' + a.toString(16) + ' 1');
    const l = t.split('\n').filter(x => x.includes(':'));
    const m = l[l.length - 1].match(/:[ ]+([0-9a-f]{8})/i);
    if (!m) throw new Error('bad reply');
    return parseInt(m[1], 16);
}
const wr = (a, v) => cmd('mww 0x' + a.toString(16) + ' 0x' + ((v >>> 0).toString(16)));
const delay = ms => new Promise(r => setTimeout(r, ms));

(async () => {
    await new Promise(r => s.once('connect', r));
    await delay(200);
    const A = {lost: sym('g_can_rx_lost'), frames: sym('g_can_rx_frames'),
               missed: sym('g_can_seq_missed'), gap: sym('g_can_gap_max_ms')};
    const before = {lost: await rd(A.lost), frames: await rd(A.frames),
                    missed: await rd(A.missed), gap: await rd(A.gap)};
    if (!(await rd(ISER1) & BIT)) throw new Error('FDCAN1_IT0 is already masked');
    try {
        restoreNeeded = true;
        await wr(ICER1, BIT);
        if (await rd(ISER1) & BIT) throw new Error('mask was not applied');
        await delay(maskMs);
    } finally {
        if (restoreNeeded) { await wr(ISER1, BIT); restoreNeeded = false; }
    }
    const enabled = !!(await rd(ISER1) & BIT);
    await delay(500);
    const after = {lost: await rd(A.lost), frames: await rd(A.frames),
                   missed: await rd(A.missed), gap: await rd(A.gap)};
    console.log(JSON.stringify({maskMs, irqRestored: enabled, before, after,
        lostDelta: after.lost - before.lost,
        framesDelta: after.frames - before.frames,
        missedDelta: after.missed - before.missed}));
    s.end();
})().catch(e => {console.error(e.message); s.destroy(); process.exitCode = 1;});
