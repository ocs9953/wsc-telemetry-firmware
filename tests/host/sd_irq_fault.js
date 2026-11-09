const fs = require('fs');
const net = require('net');
const path = require('path');
const crypto = require('crypto');
const inject = process.argv.includes('--inject-sd-irq');
const holdMs = Number(process.argv.find(x=>x.startsWith('--hold-ms='))?.slice(10) || 0);
const repeatMask = process.argv.includes('--repeat-mask');
const outputDir = process.argv.find(x => x.startsWith('--output='))?.slice(9) || 'build/fault_validation';
fs.mkdirSync(outputDir, {recursive:true});
const buildDir = process.argv.find(x=>x.startsWith('--build='))?.slice(8) || 'build/release';
const map = fs.readFileSync(path.join(buildDir,'WSC_Transmitter_Code_V1.map'),'utf8');
function symbol(name) {
    const match = map.match(new RegExp('0x([0-9a-f]+)\\s+' + name + '\\s*(?:\\r?\\n)', 'i'));
    if (!match) throw new Error('Missing symbol: ' + name);
    return parseInt(match[1],16);
}
const symbols = Object.fromEntries(['uwTick','g_can_rx_frames','g_mcp_rx_frames','g_sd_media_ok',
    'g_sd_recover_ok','g_sd_io_timeout','g_sd_hard_reset','g_sd_flush_err','g_sd_hw_ok','g_sd_recover_req',
    'g_sd_timeout_run','g_uart_q_drops','g_sd_q_drops','g_sd_discarded', 'g_sd_write_lost',
    'g_can_gap_max_ms','g_can_rx_lost','g_can_seq_missed','g_can_seq_frames',
    'g_mcp_gap_max_ms','g_mcp_rx_ovf','g_mcp_seq_missed','g_mcp_seq_frames'].map(k=>[k,symbol(k)]));
const ISER2=0xe000e108, ICER2=0xe000e188, IRQ_MASK=1<<14;
const socket=net.connect(4444,'127.0.0.1');
let pending=null, buffer='', restoreNeeded=false, stopping=false;
const start=performance.now();
function record(event,fields={}) {
    const data={utc:Date.now(),elapsedMs:Math.round(performance.now()-start),event,...fields};
    fs.appendFileSync(path.join(outputDir,'sd_irq.jsonl'),JSON.stringify(data)+'\n');
    if(event!=='sample') console.log(JSON.stringify(data));
    return data;
}
socket.on('data',chunk=>{
    buffer+=chunk.toString().replace(/\0/g,'');
    const at = pending ? buffer.indexOf(pending.echo) : -1;
    const tail = at >= 0 ? buffer.slice(at + pending.echo.length) : '';
    if(pending && at >= 0 && /\r?\n\r?>\s*$/.test(tail) &&
       (!pending.expected || pending.expected.test(tail) || /failed|error|invalid/i.test(tail))) {
        const p=pending; pending=null; clearTimeout(p.timer); p.resolve(tail);
    }
});
socket.on('error',error=>{stopping=true;if(pending){clearTimeout(pending.timer);pending.reject(error);pending=null;}});
function command(text) {
    if(pending) throw new Error('Concurrent debugger command');
    return new Promise((resolve,reject)=>{
        buffer='';
        const t=text;
        pending={resolve,reject,echo:text,expected:(/^md[wh] 0x([0-9a-f]+)/i.test(t) ? new RegExp('0x0*' + t.match(/^md[wh] 0x([0-9a-f]+)/i)[1] + ':[ ]+[0-9a-f]', 'i') : null),timer:setTimeout(()=>{pending=null;reject(new Error('Debugger timeout: '+text));},2000)};
        socket.write(text+'\n');
    });
}
async function read(address) {
    const text=await command('mdw 0x'+address.toString(16)+' 1');
    const match=text.match(new RegExp('0x'+address.toString(16)+':\\s+([0-9a-f]{8})','i'));
    if(!match) throw new Error('Invalid debugger reply: '+text);
    return parseInt(match[1],16);
}
async function write(address,value) {
    const text=await command('mww 0x'+address.toString(16)+' 0x'+value.toString(16));
    if(/error|failed|invalid/i.test(text)) throw new Error(text);
}
async function sample() {
    const values={};
    for(const [name,address] of Object.entries(symbols)) {
        values[name]=await read(address);
        if(['g_sd_media_ok','g_sd_hw_ok','g_sd_recover_req'].includes(name)) values[name]&=255;
    }
    values.sdIrqEnabled=!!((await read(ISER2))&IRQ_MASK);
    values.sdSta=await read(0x420c8034);
    values.sdIdma=await read(0x420c8050);
    return record('sample',values);
}
const delay=ms=>new Promise(resolve=>setTimeout(resolve,ms));
process.on('SIGINT',()=>{stopping=true;});
(async()=>{
    await new Promise((resolve,reject)=>{socket.once('connect',resolve);socket.once('error',reject);});
    await delay(300);
    record('start',{inject,symbols,elfSha256:crypto.createHash('sha256').update(fs.readFileSync(path.join(buildDir,'WSC_Transmitter_Code_V1.elf'))).digest('hex')});
    try {
        const baseline=await sample();
        if(!baseline.sdIrqEnabled || !baseline.g_sd_media_ok || !baseline.g_sd_hw_ok) throw new Error('Board is not in healthy SD state');
        const baseTime=performance.now();
        let injectedAt=null,restoredAt=null;
        while(!stopping && performance.now()-baseTime < 35000 + holdMs) {
            const v=await sample();
            if(v.uwTick<baseline.uwTick) throw new Error('Board reset detected');
            if(inject && injectedAt===null && performance.now()-baseTime>=5000) {
                restoreNeeded=true;
                record('irq_disable_requested',{boardTick:v.uwTick});
                await write(ICER2,IRQ_MASK);
                injectedAt=performance.now();
                if((await read(ISER2))&IRQ_MASK) throw new Error('IRQ mask was not applied');
                record('irq_disabled',{boardTick:v.uwTick});
            }
            const holdElapsed = injectedAt!==null && (performance.now()-injectedAt);
            const doRestore = holdMs > 0
                ? (holdElapsed >= holdMs)
                : (v.g_sd_io_timeout>baseline.g_sd_io_timeout || holdElapsed>8000);
            if(restoreNeeded && repeatMask && !doRestore) {
                await write(ICER2,IRQ_MASK);
            }
            if(restoreNeeded && doRestore) {
                await write(ISER2,IRQ_MASK);
                restoreNeeded=false; restoredAt=performance.now();
                record('irq_restored',{timeoutObserved:v.g_sd_io_timeout>baseline.g_sd_io_timeout,boardTick:v.uwTick});
            }
            if(injectedAt!==null && restoredAt!==null && performance.now()-restoredAt>18000) break;
            await delay(100);
        }
        const end=await sample();
        record('result',{baseline,end,timeoutDelta:end.g_sd_io_timeout-baseline.g_sd_io_timeout,
            recoveryDelta:end.g_sd_recover_ok-baseline.g_sd_recover_ok,hardResetDelta:end.g_sd_hard_reset-baseline.g_sd_hard_reset,
            canADelta:end.g_can_rx_frames-baseline.g_can_rx_frames,canBDelta:end.g_mcp_rx_frames-baseline.g_mcp_rx_frames,
            uartDropDelta:end.g_uart_q_drops-baseline.g_uart_q_drops,
            sdDropDelta:end.g_sd_q_drops-baseline.g_sd_q_drops,
            sdDiscardDelta:end.g_sd_discarded-baseline.g_sd_discarded,
            sdWriteLostDelta:end.g_sd_write_lost-baseline.g_sd_write_lost,
            canAGapMaxMs:end.g_can_gap_max_ms,canBGapMaxMs:end.g_mcp_gap_max_ms,
            canALost:end.g_can_rx_lost,canBOverflow:end.g_mcp_rx_ovf,
            canASeqMissed:end.g_can_seq_missed,canASeqFrames:end.g_can_seq_frames,
            canBSeqMissed:end.g_mcp_seq_missed,canBSeqFrames:end.g_mcp_seq_frames});
    } finally {
        if(restoreNeeded) { await write(ISER2,IRQ_MASK); record('finally_irq_restored',{enabled:!!((await read(ISER2))&IRQ_MASK)}); restoreNeeded=false; }
        socket.end();
    }
})().catch(error=>{record('error',{message:error.message,restoreNeeded});socket.destroy();process.exitCode=1;});
