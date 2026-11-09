const fs = require('fs'), path = require('path'), os = require('os'), crypto = require('crypto');
const {connect} = require('./ocd_telnet');

const buildDir = process.argv.slice(2).find(a => !a.startsWith('--')) || 'build/bench';
const noCan = process.argv.includes('--no-can');
const map = fs.readFileSync(path.join(buildDir, 'WSC_Transmitter_Code_V1.map'), 'utf8');
function sym(n) {
    const m = map.match(new RegExp('0x([0-9a-f]+)\\s+' + n + '\\s*(?:\\r?\\n)', 'i'));
    if (!m) throw new Error('missing symbol ' + n);
    return parseInt(m[1], 16);
}
const RCC_AHB2ENR1 = 0x46020c8c, SDMMC1EN = (1 << 27) >>> 0;
const ISER1 = 0xe000e104, ISER2 = 0xe000e108;
const FDCAN1_BIT = (1 << (39 - 32)) >>> 0, SDMMC1_BIT = (1 << (78 - 64)) >>> 0;
const CSR_FLAGS = [[29, 'IWDG'], [30, 'WWDG'], [28, 'SOFT'], [26, 'PIN'], [27, 'BOR'], [31, 'LPWR'], [25, 'OBL']];
const decodeReset = v => CSR_FLAGS.filter(([b]) => v & (1 << b)).map(([, n]) => n).join('+') || 'none';
const delay = ms => new Promise(r => setTimeout(r, ms));

const results = [];
const check = (name, ok, detail) => { results.push({name, ok, detail}); };

(async () => {
    let ocd;
    try { ocd = await connect(); }
    catch (e) { console.error('NO-GO: OpenOCD not reachable on 4444 (' + e.message + ')'); process.exit(1); }

    const version = await ocd.cmd('version', {expect: /Open On-Chip Debugger|MOCK/i});
    if (/MOCK/.test(version) && !process.env.PREFLIGHT_ALLOW_MOCK) {
        console.error('NO-GO: port 4444 is mock_openocd.js, not a board. Stop it (node process) and rerun.');
        ocd.close();
        process.exit(1);
    }
    if (/MOCK/.test(version)) console.log('(running against the mock - results say nothing about hardware)\n');

    const bin = path.join(buildDir, 'WSC_Transmitter_Code_V1.bin');
    if (fs.existsSync(bin)) {
        const expected = fs.readFileSync(bin);
        const dump = path.join(os.tmpdir(), 'preflight_readback_' + process.pid + '_' + Date.now() + '.bin');
        try { fs.unlinkSync(dump); } catch (e) {  }
        const reply = await ocd.cmd('dump_image "' + dump.replace(/\\/g, '/') + '" 0x08000000 ' + expected.length,
                                    {expect: /dumped \d+ bytes/i, timeoutMs: 30000});
        const said = reply.match(/dumped (\d+) bytes/i);
        const actual = fs.existsSync(dump) ? fs.readFileSync(dump) : null;
        try { fs.unlinkSync(dump); } catch (e) {  }
        if (!said || Number(said[1]) !== expected.length || !actual || actual.length !== expected.length) {
            check('flashed image matches ' + buildDir, false,
                  'readback failed - ' + (said ? 'dumped ' + said[1] + ' of ' + expected.length + ' bytes'
                                               : reply.trim().split('\n').slice(-2).join(' ')));
        } else {
            const a = crypto.createHash('sha256').update(expected).digest('hex');
            const b = crypto.createHash('sha256').update(actual).digest('hex');
            check('flashed image matches ' + buildDir, a === b,
                  a === b ? a.slice(0, 16) : 'expected ' + a.slice(0, 16) + ', found ' + b.slice(0, 16));
        }
    } else {
        check('flashed image matches ' + buildDir, false, 'no .bin beside the ELF - cannot compare');
    }

    const csr = await ocd.read32(sym('g_boot_reset_csr'));
    const mediaOk = (await ocd.read32(sym('g_sd_media_ok'))) & 255;
    const recoverReq = (await ocd.read32(sym('g_sd_recover_req'))) & 255;
    const timeoutRun = await ocd.read32(sym('g_sd_timeout_run'));
    check('SD mounted and not recovering', mediaOk === 1 && recoverReq === 0 && timeoutRun === 0,
          'media_ok=' + mediaOk + ' recover_req=' + recoverReq + ' timeout_run=' + timeoutRun);

    const enr = await ocd.read32(RCC_AHB2ENR1);
    check('SDMMC clock enabled', (enr & SDMMC1EN) !== 0, 'AHB2ENR1=0x' + enr.toString(16));
    const iser2 = await ocd.read32(ISER2);
    check('SDMMC interrupt enabled', (iser2 & SDMMC1_BIT) !== 0, 'ISER2=0x' + iser2.toString(16));
    const iser1 = await ocd.read32(ISER1);
    check('FDCAN1 interrupt enabled', (iser1 & FDCAN1_BIT) !== 0, 'ISER1=0x' + iser1.toString(16));

    const tick1 = await ocd.read32(sym('uwTick'));
    const canA1 = await ocd.read32(sym('g_can_rx_frames')), canB1 = await ocd.read32(sym('g_mcp_rx_frames'));
    const hb1 = await ocd.read32(sym('g_can_seq_frames'));
    await delay(1000);
    const tick2 = await ocd.read32(sym('uwTick'));
    const canA2 = await ocd.read32(sym('g_can_rx_frames')), canB2 = await ocd.read32(sym('g_mcp_rx_frames'));
    const hb2 = await ocd.read32(sym('g_can_seq_frames'));
    const advanced = (tick2 - tick1) >>> 0;
    check('board running', tick2 > tick1 && advanced >= 500 && advanced <= 5000,
          'uptime ' + (tick2 / 1000).toFixed(1) + 's, advanced ' + advanced + 'ms in ~1s, last reset ' + decodeReset(csr));
    check('CAN bus A carrying traffic', canA2 > canA1, (canA2 - canA1) + ' frames/s');
    check('CAN bus B carrying traffic', canB2 > canB1, (canB2 - canB1) + ' frames/s');
    check('sequence heartbeat present', hb2 > hb1, (hb2 - hb1) + ' beats/s (0 means no dummy has DUMMY_HEARTBEAT_ID set)');

    const THREADS = ['MCP_RX_Thread', 'UART_Thread', 'UART_RX_Thread', 'SD_Logger_Thread',
                     'VcuLcd_Thread', 'GPS_Thread', 'ADC_Thread', 'fx_app_thread', '_tx_timer_thread'];
    const inImage = n => new RegExp('^\\s+0x[0-9a-f]+\\s+' + n + '\\s*$', 'im').test(map);
    const thrExpected = THREADS.filter(inImage).length;
    const sem = await ocd.read32(sym('_tx_semaphore_created_count'));
    const thr = await ocd.read32(sym('_tx_thread_created_count'));
    check('RTOS objects at baseline', sem === 7 && thr === thrExpected,
          'semaphores ' + sem + '/7, threads ' + thr + '/' + thrExpected);

    ocd.close();

    const width = Math.max(...results.map(r => r.name.length));
    for (const r of results) console.log((r.ok ? '  ok   ' : '  FAIL ') + r.name.padEnd(width) + '  ' + r.detail);
    const advisory = ['CAN bus B carrying traffic', 'sequence heartbeat present']
        .concat(noCan ? ['CAN bus A carrying traffic'] : []);
    const blocking = results.filter(r => !r.ok && !advisory.includes(r.name));
    const warned = results.filter(r => !r.ok && advisory.includes(r.name));
    if (warned.length) console.log('\nnote: ' + warned.map(r => r.name).join(', ') + ' - not blocking');
    if (!noCan && results.some(r => r.name === 'CAN bus A carrying traffic' && !r.ok))
        console.log('note: bus A is silent. Connect CAN, or rerun with --no-can (-NoCan) for work that does not need it.');
    console.log(blocking.length ? '\nNO-GO\n' : '\nGO\n');
    process.exit(blocking.length ? 1 : 0);
})().catch(e => { console.error('NO-GO: ' + e.message); process.exit(1); });
