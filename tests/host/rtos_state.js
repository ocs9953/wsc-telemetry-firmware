const fs = require('fs'), net = require('net'), path = require('path');
const buildDir = process.argv[2] || 'build/bench';
const map = fs.readFileSync(path.join(buildDir, 'WSC_Transmitter_Code_V1.map'), 'utf8');
function sym(n) {
    const m = map.match(new RegExp('0x([0-9a-f]+)\\s+' + n + '\\s*(?:\\r?\\n)', 'i'));
    if (!m) throw new Error('missing ' + n);
    return parseInt(m[1], 16);
}
const Q_ENQUEUED = 16, Q_AVAILABLE = 20;

const s = net.connect(4444, '127.0.0.1');
let buf = '', pending = null;
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
(async () => {
    await new Promise(r => s.once('connect', r));
    await new Promise(r => setTimeout(r, 200));
    const uq = sym('UART_Queue'), sq = sym('SD_Queue');
    const o = {
        uwTick: await rd(sym('uwTick')),
        semaphores: await rd(sym('_tx_semaphore_created_count')),
        threads: await rd(sym('_tx_thread_created_count')),
        uartEnqueued: await rd(uq + Q_ENQUEUED),
        uartAvailable: await rd(uq + Q_AVAILABLE),
        sdEnqueued: await rd(sq + Q_ENQUEUED),
        sdAvailable: await rd(sq + Q_AVAILABLE),
        uartQueueDrops: await rd(sym('g_uart_q_drops')),
        sdQueueDrops: await rd(sym('g_sd_q_drops')),
        sdDiscarded: await rd(sym('g_sd_discarded')),
        sdWriteLost: await rd(sym('g_sd_write_lost')),
        sdMediaOk: (await rd(sym('g_sd_media_ok'))) & 255,
        sdRecoverReq: (await rd(sym('g_sd_recover_req'))) & 255,
    };
    o.sdTotalLost = o.sdDiscarded + o.sdWriteLost;
    console.log(JSON.stringify(o));
    s.end();
})().catch(e => {console.error(e.message); process.exit(1);});
