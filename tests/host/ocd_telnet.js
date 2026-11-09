const net = require('net');

const PROMPT_END = /\r?\n\r?>\s*$/;
const ERROR_TEXT = /\b(failed|error|invalid)\b/i;

function connect(port = 4444, host = '127.0.0.1') {
    return new Promise((resolve, reject) => {
        const socket = net.connect(port, host);
        let buffer = '', pending = null;

        socket.setEncoding('utf8');
        socket.on('data', chunk => {
            buffer += chunk.replace(/\0/g, '');
            if (!pending) return;
            const at = buffer.indexOf(pending.echo);
            if (at < 0) return;
            const after = buffer.slice(at + pending.echo.length);
            if (!PROMPT_END.test(after)) return;
            if (pending.expect && !pending.expect.test(after) && !ERROR_TEXT.test(after)) return;
            const p = pending; pending = null; clearTimeout(p.timer);
            p.resolve(after);
        });
        socket.once('error', reject);
        socket.once('connect', () => {
            setTimeout(() => {
                buffer = '';
                resolve({
                    cmd(text, {expect = null, timeoutMs = 5000} = {}) {
                        if (pending) return Promise.reject(new Error('concurrent command: ' + text));
                        return new Promise((res, rej) => {
                            buffer = '';
                            pending = {echo: text, expect, resolve: res,
                                       timer: setTimeout(() => { pending = null; rej(new Error('debugger timeout: ' + text)); }, timeoutMs)};
                            socket.write(text + '\n');
                        });
                    },
                    async read32(address) {
                        const hex = (address >>> 0).toString(16);
                        const re = new RegExp('0x0*' + hex + ':[ ]+([0-9a-f]{8})', 'i');
                        for (let attempt = 0; attempt < 3; attempt++) {
                            const reply = await this.cmd('mdw 0x' + hex, {expect: re});
                            const m = reply.match(re);
                            if (m) return parseInt(m[1], 16) >>> 0;
                        }
                        throw new Error('unreadable address 0x' + hex);
                    },
                    async write32(address, value) {
                        const reply = await this.cmd('mww 0x' + (address >>> 0).toString(16) + ' 0x' + (value >>> 0).toString(16));
                        if (ERROR_TEXT.test(reply)) throw new Error(reply.trim());
                    },
                    close() { socket.end(); },
                    destroy() { socket.destroy(); },
                });
            }, 300);
        });
    });
}

module.exports = {connect, ERROR_TEXT};
