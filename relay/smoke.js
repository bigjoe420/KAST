'use strict';
/*
 * Loopback smoke test: boots the relay in-process on a test port with the
 * chat simulator running, connects a ws client, and expects the hello
 * frame, a pong answer, and one simulated chat frame. Exits non-zero on
 * failure. Run: npm test (from relay/).
 */
process.env.KAST_RELAY_PORT = '18765';
process.env.KAST_OBS_ENABLED = '0';
process.env.KAST_KICK_ENABLED = '0';

const { spawn } = require('child_process');
const WebSocket = require('ws');

const srv = spawn(process.execPath, ['server.js', '--simulate-chat'], {
  cwd: __dirname,
  stdio: 'pipe',
});
let srv_log = '';
srv.stdout.on('data', (d) => { srv_log += d.toString(); });
srv.stderr.on('data', (d) => { srv_log += d.toString(); });

function fail(why) {
  console.error(`SMOKE FAIL: ${why}\n--- server log ---\n${srv_log}`);
  srv.kill();
  process.exit(1);
}

setTimeout(() => {
  const ws = new WebSocket('ws://127.0.0.1:18765');
  const got = { hello: false, pong: false, chat: false };
  const timer = setTimeout(() => fail('timeout waiting for hello/pong/chat'), 15000);

  ws.on('open', () => ws.send(JSON.stringify({ type: 'ping' })));
  ws.on('message', (d) => {
    let m;
    try {
      m = JSON.parse(d.toString());
    } catch {
      return fail(`garbage frame: ${d.toString().slice(0, 60)}`);
    }
    if (m.type === 'hello') got.hello = true;
    if (m.type === 'pong') got.pong = true;
    if (m.type === 'chat') got.chat = true;
    if (got.hello && got.pong && got.chat) {
      clearTimeout(timer);
      console.log('SMOKE PASS: hello + pong + chat frame all received');
      ws.close();
      srv.kill();
      process.exit(0);
    }
  });
  ws.on('error', (e) => fail(e.message));
}, 1500);
