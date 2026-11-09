const fs = require('fs'), net = require('net'), path = require('path');
const positional = process.argv.slice(2).filter(a => !a.startsWith('--'));
const scenario = positional[0] || 'healthy';
const buildDir = positional[1] || 'build/bench';
const noisy = process.argv.includes('--noisy');
const map = fs.readFileSync(path.join(buildDir, 'WSC_Transmitter_Code_V1.map'), 'utf8');
const bin = fs.readFileSync(path.join(buildDir, 'WSC_Transmitter_Code_V1.bin'));
function sym(n) {
    const m = map.match(new RegExp('0x([0-9a-f]+)\\s+' + n + '\\s*(?:\\r?\\n)', 'i'))
        || map.match(new RegExp('\\.bss\\.' + n + '\\s+0x([0-9a-f]+)\\s+', 'i'));
    if (!m) throw new Error('missing ' + n);
    return parseInt(m[1], 16);
}

const mem = new Map();
const setByte = (a, v) => mem.set(a >>> 0, v & 0xff);
const getByte = a => mem.get(a >>> 0) || 0;
const set = (a, v) => { for (let i = 0; i < 4; i++) setByte(a + i, (v >>> (8 * i)) & 0xff); };
const get = a => (getByte(a) | (getByte(a + 1) << 8) | (getByte(a + 2) << 16) | (getByte(a + 3) << 24)) >>> 0;

const started = Date.now();
const UWTICK = sym('uwTick');
set(sym('g_boot_reset_csr'), 0x04000000);
set(sym('g_sd_media_ok'), 1);
set(sym('g_sd_hw_ok'), 1);
set(0x46020c8c, 0xc800049f);
set(0xe000e104, 1 << 7);
set(0xe000e108, 1 << 14);
set(0x420c8000, 3);
set(sym('_tx_semaphore_created_count'), 7);
set(sym('_tx_thread_created_count'), 8);
const live = {};
live[sym('g_can_rx_frames')] = 58;
live[sym('g_mcp_rx_frames')] = 5;
live[sym('g_can_seq_frames')] = 10;
for (const a of Object.keys(live)) set(Number(a), 1000);

switch (scenario) {
    case 'healthy': case 'image_mismatch': case 'dump_fails': break;
    case 'halted': for (const a of Object.keys(live)) live[a] = 0; break;
    case 'sd_recovering': set(sym('g_sd_media_ok'), 0); set(sym('g_sd_recover_req'), 1); break;
    case 'clock_masked': set(0x46020c8c, 0xc000049f); break;
    case 'can_silent': for (const a of Object.keys(live)) live[a] = 0; break;
    case 'rtos_leak': set(sym('_tx_semaphore_created_count'), 5); break;
    default: console.error('unknown scenario ' + scenario); process.exit(2);
}

function write32(a, v) {
    if (a >= 0xe000e180 && a < 0xe000e1a0) { const iser = a - 0x80; set(iser, get(iser) & ~v); return; }
    if (a >= 0xe000e100 && a < 0xe000e120) { set(a, get(a) | v); return; }
    set(a, v);
}
function read32(a) {
    if (a === UWTICK) return scenario === 'halted' ? 123456 : (123456 + (Date.now() - started)) >>> 0;
    if (live[a] !== undefined) set(a, get(a) + live[a]);
    return get(a);
}

function dump(addr, count, half) {
    const step = half ? 2 : 4, width = half ? 4 : 8, perLine = 8;
    let out = '';
    for (let i = 0; i < count; i += perLine) {
        const lineAddr = addr + i * step;
        const words = [];
        for (let k = i; k < Math.min(count, i + perLine); k++) {
            const a = addr + k * step;
            const v = half ? (getByte(a) | (getByte(a + 1) << 8)) : read32(a);
            words.push(v.toString(16).padStart(width, '0'));
        }
        out += '0x' + lineAddr.toString(16).padStart(8, '0') + ': ' + words.join(' ') + ' \r\n';
    }
    return out;
}

function reply(line) {
    const echo = line + '\r\n';
    let m;
    if (/^version$/.test(line)) return echo + 'MOCK OpenOCD (tests/host/mock_openocd.js) - not a board\r\n';
    if ((m = line.match(/^(mdw|mdh) 0x([0-9a-f]+)(?: (\d+))?$/i))) {
        return echo + dump(parseInt(m[2], 16) >>> 0, Number(m[3] || 1), m[1].toLowerCase() === 'mdh');
    }
    if ((m = line.match(/^(mww|mwh) 0x([0-9a-f]+) (0x[0-9a-f]+|\d+)(?: (\d+))?$/i))) {
        const half = m[1].toLowerCase() === 'mwh';
        const addr = parseInt(m[2], 16) >>> 0;
        const value = Number(m[3]) >>> 0;
        const count = Number(m[4] || 1);
        for (let k = 0; k < count; k++) {
            if (half) { setByte(addr + 2 * k, value); setByte(addr + 2 * k + 1, value >>> 8); }
            else write32(addr + 4 * k, value);
        }
        return echo;
    }
    if ((m = line.match(/^dump_image\s+"?([^"\s]+)"?\s+0x08000000\s+(\d+)$/i))) {
        if (scenario === 'dump_fails') return echo + 'Error: Failed to read memory at 0x08000000\r\n';
        const data = Buffer.from(bin.subarray(0, Number(m[2])));
        if (scenario === 'image_mismatch') data[100] ^= 0xff;
        fs.writeFileSync(m[1], data);
        return echo + 'dumped ' + data.length + ' bytes in 0.5s\r\n';
    }
    if (/^program /.test(line)) {
        return echo + '** Programming Started **\r\n** Programming Finished **\r\n** Verify Started **\r\n** Verified OK **\r\n';
    }
    if (/^(reset|targets|halt|resume)/.test(line)) return echo;
    return echo + 'invalid command name "' + line.split(' ')[0] + '"\r\n';
}

let served = 0;
const wait = ms => new Promise(r => setTimeout(r, ms));
const server = net.createServer(sock => {
    sock.write('MOCK OpenOCD - not a board\r\n> ');
    let buf = '', chain = Promise.resolve();
    sock.on('data', d => {
        buf += d.toString();
        let i;
        while ((i = buf.indexOf('\n')) >= 0) {
            const line = buf.slice(0, i).trim();
            buf = buf.slice(i + 1);
            if (!line) continue;
            const noisyTurn = noisy && (++served % 3 === 0);
            chain = chain.then(async () => {
                const full = reply(line);
                const echo = line + '\r\n';
                sock.write(echo);
                if (noisyTurn) {
                    await wait(15);
                    sock.write("Info : accepting 'telnet' connection on tcp/4444\r\n\r> ");
                }
                await wait(noisyTurn ? 15 : 0);
                sock.write(full.slice(echo.length) + '\r> ');
            }).catch(() => {});
        }
    });
    sock.on('error', () => {});
});
server.listen(4444, '127.0.0.1', () =>
    console.log('mock OpenOCD on 4444, scenario ' + scenario + (noisy ? ', noisy' : '')));
