const fs = require('fs'), net = require('net'), path = require('path');
const buildDir = process.argv[2] || 'build/bench';
const map = fs.readFileSync(path.join(buildDir, 'WSC_Transmitter_Code_V1.map'), 'utf8');
function sym(n) {
    const m = map.match(new RegExp('0x([0-9a-f]+)\\s+' + n + '\\s*(?:\\r?\\n)', 'i'));
    if (!m) throw new Error('missing ' + n);
    return parseInt(m[1], 16);
}
const names = ['uwTick', 'g_can_rx_frames', 'g_can_short_frames', 'g_can_rx_lost', 'g_can_gap_max_ms',
    'g_can_seq_frames', 'g_can_seq_missed', 'g_can_seq_restarts', 'g_can_busoff_count', 'g_can_busoff_recover',
    'g_mcp_rx_frames', 'g_mcp_rx_ovf', 'g_mcp_gap_max_ms', 'g_mcp_seq_frames', 'g_mcp_seq_missed',
    'g_mcp_tec', 'g_mcp_rec', 'g_mcp_rx_last_id', 'g_sd_media_ok', 'g_sd_discarded'];
const A = Object.fromEntries(names.map(n => [n, sym(n)]));

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
    const o = {};
    for (const [k, v] of Object.entries(A)) o[k] = await rd(v);
    o.g_sd_media_ok &= 255;
    o.g_mcp_rx_last_id = '0x' + o.g_mcp_rx_last_id.toString(16);
    console.log(JSON.stringify(o));
    s.end();
})().catch(e => {console.error(e.message); process.exit(1);});
