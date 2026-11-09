const {connect} = require('./ocd_telnet');
const commands = process.argv.slice(2);
if (!commands.length) { console.error('usage: ocd_cmd.js <command> [command ...]'); process.exit(2); }

(async () => {
    let ocd;
    try { ocd = await connect(); }
    catch (e) { console.error('OpenOCD not reachable on 4444: ' + e.message); process.exit(1); }
    for (const c of commands) {
        const slow = /^(program|reset|halt|dump_image)/.test(c);
        const expect = /^program /.test(c)
            ? /\*\* (Verified OK|Verify Failed|Programming Failed) \*\*/ : null;
        const reply = await ocd.cmd(c, {expect, timeoutMs: slow ? 90000 : 5000});
        process.stdout.write('> ' + c + '\n' + reply.split('\n')
            .filter(l => /[a-z0-9]/i.test(l) && !/^\s*>\s*$/.test(l))
            .join('\n') + '\n');
    }
    ocd.close();
})().catch(e => { console.error(e.message); process.exit(1); });
