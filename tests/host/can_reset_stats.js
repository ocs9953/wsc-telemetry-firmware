const fs = require('fs'), path = require('path');
const buildDir = process.argv[2] || 'build/bench';
const map = fs.readFileSync(path.join(buildDir, 'WSC_Transmitter_Code_V1.map'), 'utf8');
function sym(n) {
    const m = map.match(new RegExp('0x([0-9a-f]+)\\s+' + n + '\\s*(?:\\r?\\n)', 'i'))
        || map.match(new RegExp('\\.bss\\.' + n + '\\s+0x([0-9a-f]+)\\s+', 'i'));
    if (!m) throw new Error('missing ' + n);
    return parseInt(m[1], 16);
}
function sizeOf(n) {
    const m = map.match(new RegExp('\\.bss\\.' + n + '\\s+0x[0-9a-f]+\\s+0x([0-9a-f]+)', 'i'));
    if (!m) throw new Error('size of ' + n + ' not found in map');
    return parseInt(m[1], 16);
}

const zeroWords = ['g_can_gap_max_ms', 'g_can_gap_last_ms', 'g_can_seq_frames', 'g_can_seq_missed',
    'g_can_seq_restarts', 'g_can_rx_lost', 'g_can_short_frames',
    'g_mcp_gap_max_ms', 'g_mcp_gap_last_ms', 'g_mcp_seq_frames', 'g_mcp_seq_missed',
    'g_mcp_seq_restarts', 'g_mcp_rx_ovf'];
const countWords = sizeOf('g_fresh_count') / 4;
const gapHalves = sizeOf('g_fresh_gap_max') / 2;
if (countWords !== gapHalves) throw new Error('freshness arrays disagree: ' + countWords + ' vs ' + gapHalves);

const {connect, ERROR_TEXT} = require('./ocd_telnet');

(async () => {
    let ocd;
    try { ocd = await connect(); }
    catch (e) { console.error('OpenOCD not reachable on 4444: ' + e.message); process.exit(1); }
    const write = async text => {
        const reply = await ocd.cmd(text);
        if (ERROR_TEXT.test(reply)) throw new Error(text + ' -> ' + reply.trim());
    };
    for (const n of zeroWords) await write('mww 0x' + sym(n).toString(16) + ' 0');
    await write('mww 0x' + sym('g_fresh_count').toString(16) + ' 0 ' + countWords);
    await write('mwh 0x' + sym('g_fresh_gap_max').toString(16) + ' 0 ' + gapHalves);

    const countBase = sym('g_fresh_count'), gapBase = sym('g_fresh_gap_max');
    const checks = [countBase, countBase + 4 * (countWords - 1), sym('g_can_gap_max_ms')];
    for (const a of checks) {
        const v = await ocd.read32(a);
        if (v !== 0) throw new Error('0x' + a.toString(16) + ' still ' + v + ' after reset');
    }
    const lastGapWord = await ocd.read32((gapBase + 2 * (gapHalves - 1)) & ~3);
    const lastGap = ((gapBase + 2 * (gapHalves - 1)) & 2) ? (lastGapWord >>> 16) : (lastGapWord & 0xffff);
    if (lastGap !== 0) throw new Error('last gap entry still ' + lastGap + ' after reset');

    console.log('cleared and read back: ' + zeroWords.length + ' bus counters, ' + countWords +
                ' per-frame counts and gaps (fresh_tick left as is)');
    ocd.close();
})().catch(e => { console.error(e.message); process.exit(1); });
