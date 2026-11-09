const fs=require('fs'),path=require('path');
const directory=process.argv[2]||'build/fault_validation';
const chunks=fs.readFileSync(path.join(directory,'radio_chunks.jsonl'),'utf8').trim().split(/\r?\n/).filter(Boolean).map(JSON.parse);
function decode(buf){const out=[];for(let i=0;i<buf.length;){const n=buf[i++];if(!n||i+n-1>buf.length)return null;for(let j=1;j<n;j++)out.push(buf[i++]);if(n!==255&&i<buf.length)out.push(0);}return Buffer.from(out);}
function crc8(buf){let c=0;for(const x of buf){c^=x;for(let i=0;i<8;i++)c=(c&128)?((c<<1)^7)&255:(c<<1)&255;}return c;}
let frame=[],discardFirst=true,framingErrors=0,crcErrors=0,otherFrames=0;
const errors=[];
const packets=[];
for(const chunk of chunks){for(const byte of Buffer.from(chunk.data,'base64')){
    if(byte!==0){frame.push(byte);continue;}
    if(discardFirst){frame=[];discardFirst=false;continue;}
    if(!frame.length)continue;
    const data=decode(Buffer.from(frame));frame=[];
    if(!data){framingErrors++;errors.push({utc:chunk.utc,type:'framing'});continue;}
    if(data.length!==17||data[0]!==1){otherFrames++;errors.push({utc:chunk.utc,type:'other'});continue;}
    const log=data.subarray(1);
    if(crc8(log.subarray(0,15))!==log[15]){crcErrors++;errors.push({utc:chunk.utc,type:'crc'});continue;}
    packets.push({utc:chunk.utc,ts:log.readUInt32LE(0),level:log[4],src:log[5],key:log[6],value:log.subarray(7,15).toString('hex')});
}}
function summarize(list){
    const groups={};let maxArrivalGap=0,maxBoardGap=0,backwards=0;
    for(let i=0;i<list.length;i++){
        const p=list[i];if(i){maxArrivalGap=Math.max(maxArrivalGap,p.utc-list[i-1].utc);maxBoardGap=Math.max(maxBoardGap,p.ts-list[i-1].ts);if(p.ts<list[i-1].ts)backwards++;}
        const key=p.src+':'+p.key;const g=groups[key]||(groups[key]={count:0,first:p.ts,last:p.ts,maxBoardGap:0,maxArrivalGap:0,lastArrival:p.utc});
        g.count++;g.maxBoardGap=Math.max(g.maxBoardGap,p.ts-g.last);g.maxArrivalGap=Math.max(g.maxArrivalGap,p.utc-g.lastArrival);g.last=p.ts;g.lastArrival=p.utc;
    }
    for(const g of Object.values(groups))delete g.lastArrival;
    return {count:list.length,first:list[0],last:list.at(-1),maxArrivalGap,maxBoardGap,backwards,groups};
}
const result={framingErrors,crcErrors,otherFrames,capture:summarize(packets)};
result.errors=errors;
result.steadyCapture=summarize(packets.filter(p=>p.utc>=(chunks[0]?.utc??0)+5000));
result.steadyErrors=errors.filter(e=>e.utc>=(chunks[0]?.utc??0)+5000);
const faultFile=path.join(directory,fs.existsSync(path.join(directory,'sd_irq.jsonl'))?'sd_irq.jsonl':'idle_stall.jsonl');
if(fs.existsSync(faultFile)){
    const events=fs.readFileSync(faultFile,'utf8').trim().split(/\r?\n/).filter(Boolean).map(JSON.parse);
    const fault=events.find(e=>e.event==='irq_disabled'||e.event==='clock_off');
    const outcome=events.findLast(e=>e.event==='result');
    if(fault){result.fault={event:fault,outcome};result.aroundFault=summarize(packets.filter(p=>p.utc>=fault.utc-3000&&p.utc<=fault.utc+15000));}
    if(fault&&outcome)result.faultWindow=summarize(packets.filter(p=>p.utc>=fault.utc&&p.utc<=outcome.utc));
    if(fault&&outcome)result.faultErrors=errors.filter(e=>e.utc>=fault.utc&&e.utc<=outcome.utc);
}
fs.writeFileSync(path.join(directory,'radio_analysis.json'),JSON.stringify(result,null,2));
console.log(JSON.stringify({framingErrors,crcErrors,otherFrames,totalPackets:packets.length,
    firstTs:packets[0]?.ts,lastTs:packets.at(-1)?.ts,
    aroundFault:result.aroundFault?{...result.aroundFault,groups:undefined}:undefined},null,2));
