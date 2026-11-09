const fs = require('fs');
const net = require('net');
const path = require('path');
const crypto = require('crypto');

const inject = process.argv.includes('--inject-idle-stall');
const stallMs = Number(process.argv.find(x => x.startsWith('--stall-ms='))?.slice(11) || 12000);
const buildDir = process.argv.find(x => x.startsWith('--build='))?.slice(8) || 'build/bench';
const outputDir = process.argv.find(x => x.startsWith('--output='))?.slice(9) || 'build/idle_stall';
const repeatMask = process.argv.includes('--repeat-mask');
fs.mkdirSync(outputDir, {recursive: true});

const map = fs.readFileSync(path.join(buildDir, 'WSC_Transmitter_Code_V1.map'), 'utf8');
function symbol(name) {
    const m = map.match(new RegExp('0x([0-9a-f]+)\\s+' + name + '\\s*(?:\\r?\\n)', 'i'));
    if (!m) throw new Error('Missing symbol: ' + name);
    return parseInt(m[1], 16);
}
const symbols = Object.fromEntries(['uwTick', 'g_sd_media_ok', 'g_sd_hw_ok', 'g_sd_recover_ok',
    'g_sd_io_timeout', 'g_sd_hard_reset', 'g_sd_recover_req', 'g_sd_timeout_run',
    'g_sd_discarded', 'g_sd_write_lost', 'g_boot_reset_csr'].map(k => [k, symbol(k)]));

const SDMMC = 0x420c8000;
const REG = {POWER: SDMMC + 0x00, CLKCR: SDMMC + 0x04, STA: SDMMC + 0x34};
const STA_ACTIVE = (1 << 12) | (1 << 13);

const RCC_AHB2ENR1 = 0x46020c8c;
const SDMMC1EN = 1 << 27;

const CSR_FLAGS = [[29, 'IWDG'], [30, 'WWDG'], [28, 'SOFT'], [26, 'PIN'],
                   [27, 'BOR'], [31, 'LPWR'], [25, 'OBL']];
const decodeReset = v => CSR_FLAGS.filter(([b]) => v & (1 << b)).map(([, n]) => n).join('+') || 'none';

const socket = net.connect(4444, '127.0.0.1');
let pending = null, buffer = '', restoreNeeded = false;
const start = performance.now();

function record(event, fields = {}) {
    const data = {utc: Date.now(), elapsedMs: Math.round(performance.now() - start), event, ...fields};
    fs.appendFileSync(path.join(outputDir, 'idle_stall.jsonl'), JSON.stringify(data) + '\n');
    if (event !== 'sample') console.log(JSON.stringify(data));
    return data;
}
socket.on('data', chunk => {
    buffer += chunk.toString().replace(/\0/g, '');
    const at = pending ? buffer.indexOf(pending.echo) : -1;
    const tail = at >= 0 ? buffer.slice(at + pending.echo.length) : '';
    if (pending && at >= 0 && /\r?\n\r?>\s*$/.test(tail) &&
        (!pending.expected || pending.expected.test(tail) || /failed|error|invalid/i.test(tail))) {
        const p = pending; pending = null; clearTimeout(p.timer); p.resolve(tail);
    }
});
socket.on('error', e => { if (pending) { clearTimeout(pending.timer); pending.reject(e); pending = null; } });

function command(text) {
    if (pending) throw new Error('Concurrent debugger command');
    return new Promise((resolve, reject) => {
        buffer = '';
        const t = text;
        pending = {resolve, reject, echo: text, expected: (/^md[wh] 0x([0-9a-f]+)/i.test(t) ? new RegExp('0x0*' + t.match(/^md[wh] 0x([0-9a-f]+)/i)[1] + ':[ ]+[0-9a-f]', 'i') : null),
                   timer: setTimeout(() => {pending = null; reject(new Error('Debugger timeout: ' + text));}, 3000)};
        socket.write(text + '\n');
    });
}
const delay = ms => new Promise(r => setTimeout(r, ms));

async function read(address) {
    let last = '';
    for (let attempt = 0; attempt < 3; attempt++) {
        last = await command('mdw 0x' + address.toString(16) + ' 1');
        const m = last.match(new RegExp('0x0*' + address.toString(16) + ':[ ]+([0-9a-f]{8})', 'i'));
        if (m) return parseInt(m[1], 16);
        await delay(20);
    }
    throw new Error('Invalid debugger reply: ' + last.trim());
}
async function write(address, value) {
    const t = await command('mww 0x' + address.toString(16) + ' 0x' + (value >>> 0).toString(16));
    if (/error|failed|invalid/i.test(t)) throw new Error(t);
}
async function sample() {
    const v = {};
    for (const [name, address] of Object.entries(symbols)) {
        v[name] = await read(address);
        if (['g_sd_media_ok', 'g_sd_hw_ok', 'g_sd_recover_req'].includes(name)) v[name] &= 255;
    }
    v.sdPower = await read(REG.POWER);
    v.sdSta = await read(REG.STA);
    return record('sample', v);
}

(async () => {
    await new Promise((resolve, reject) => {socket.once('connect', resolve); socket.once('error', reject);});
    await delay(300);
    record('start', {inject, stallMs, symbols,
        elfSha256: crypto.createHash('sha256').update(fs.readFileSync(path.join(buildDir, 'WSC_Transmitter_Code_V1.elf'))).digest('hex')});

    let resetSeen = false, tickAfterReset = null;
    let clockBackAt = null, outageMs = null, regates = 0;
    try {
        const baseline = await sample();
        if (!baseline.g_sd_media_ok || !baseline.g_sd_hw_ok) throw new Error('Board is not in healthy SD state');
        if (baseline.sdPower !== 3) throw new Error('SDMMC is not powered: 0x' + baseline.sdPower.toString(16));
        record('boot_reset_cause', {raw: baseline.g_boot_reset_csr, cause: decodeReset(baseline.g_boot_reset_csr)});

        let injectedAt = null;
        if (inject) {
            let waited = 0;
            while ((await read(REG.STA)) & STA_ACTIVE) {
                if ((waited += 20) > 3000) throw new Error('SDMMC never went idle');
                await delay(20);
            }
            restoreNeeded = true;
            const enr = await read(RCC_AHB2ENR1);
            await write(RCC_AHB2ENR1, enr & ~SDMMC1EN);
            injectedAt = performance.now();
            record('clock_off', {boardTick: await read(symbols.uwTick),
                enrBefore: enr.toString(16), enrAfter: (await read(RCC_AHB2ENR1)).toString(16)});
        }

        let last = baseline.uwTick;
        while (performance.now() - start < 8000 + stallMs) {
            const tick = await read(symbols.uwTick);
            if (tick < last) {
                resetSeen = true;
                tickAfterReset = tick;
                record('board_reset', {previousTick: last, tick,
                    sinceInjectMs: injectedAt ? Math.round(performance.now() - injectedAt) : null});
                break;
            }
            last = tick;

            if (injectedAt !== null) {
                const on = ((await read(RCC_AHB2ENR1)) & SDMMC1EN) !== 0;
                if (on && clockBackAt === null) {
                    clockBackAt = performance.now();
                    outageMs = Math.round(clockBackAt - injectedAt);
                    record('clock_back_on_by_firmware', {outageMs});
                }
                if (on && repeatMask) {
                    await write(RCC_AHB2ENR1, (await read(RCC_AHB2ENR1)) & ~SDMMC1EN);
                    regates++;
                }
            }

            if (injectedAt && performance.now() - injectedAt >= stallMs) break;
            await delay(150);
        }

        if (restoreNeeded) {
            await write(RCC_AHB2ENR1, (await read(RCC_AHB2ENR1)) | SDMMC1EN);
            restoreNeeded = false;
            record('clock_restored', {enr: (await read(RCC_AHB2ENR1)).toString(16)});
        }
        for (let i = 0; i < 40; i++) {
            const v = await sample();
            if (v.g_sd_media_ok && !v.g_sd_recover_req) break;
            await delay(500);
        }
        const end = await sample();
        record('result', {
            verdict: resetSeen ? 'WATCHDOG RESET - idle entry is not covered'
                               : 'no reset - survived the idle entry stall',
            resetSeen, tickAfterReset, baseline, end,
            resetCauseAfter: decodeReset(end.g_boot_reset_csr),
            measuredOutageMs: outageMs, clockRegates: regates, repeatMask,
            timeoutDelta: end.g_sd_io_timeout - baseline.g_sd_io_timeout,
            recoveryDelta: end.g_sd_recover_ok - baseline.g_sd_recover_ok,
            hardResetDelta: end.g_sd_hard_reset - baseline.g_sd_hard_reset,
            discardDelta: end.g_sd_discarded - baseline.g_sd_discarded,
            writeLostDelta: end.g_sd_write_lost - baseline.g_sd_write_lost});
    } finally {
        if (restoreNeeded) {
            await write(RCC_AHB2ENR1, (await read(RCC_AHB2ENR1)) | SDMMC1EN);
            record('finally_clock_restored', {enr: (await read(RCC_AHB2ENR1)).toString(16)});
        }
        socket.end();
    }
})().catch(e => {record('error', {message: e.message, restoreNeeded}); socket.destroy(); process.exitCode = 1;});
