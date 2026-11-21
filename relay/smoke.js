'use strict';
/*
 * Loopback smoke test: boots the relay in-process on a test port with the
 * chat simulator running, connects a ws client, and expects the hello
 * frame, a pong answer, and one simulated chat frame. Then reconnects a
 * second "box" socket from the same address and expects the stale one to
 * be superseded (close 4000) while the new one keeps working.
 * Exits non-zero on failure. Run: npm test (from relay/).
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

const pass = (why) => {
  console.log(`SMOKE PASS: ${why}`);
  srv.kill();
  process.exit(0);
};

setTimeout(() => {
  const ws = new WebSocket('ws://127.0.0.1:18765');
  const got = { hello: false, pong: false, chat: false };
  const timer = setTimeout(() => fail('timeout waiting for hello/pong/chat'), 15000);
  const dup = { oldClosed: false, pong2: false, started: false, timer2: null };
  dup.finish = () => {
    if (!dup.oldClosed || !dup.pong2) return;
    clearTimeout(dup.timer2);
    if (!srv_log.includes('duplicate device socket')) {
      fail('server did not log the duplicate socket');
    }
    pass('hello + pong + chat received; duplicate device socket superseded');
  };

  /* Phase 2 — the box reconnects without dropping its old socket: the
     relay must close the stale socket (4000 superseded) and keep serving
     the new one. `ws` plays the old box socket. Registration order is
     forced through a ping: the server processes a socket's frames in
     order, so the pong for the ping after dev.hello proves the old socket
     is the registered device before the new one introduces itself. */
  let ws2 = null;
  const startDupCheck = () => {
    if (dup.started) return;   /* later sim-chat frames must not restart this */
    dup.started = true;
    dup.timer2 = setTimeout(() => fail('timeout in duplicate-device check'), 10000);
    ws.send(JSON.stringify({ type: 'dev.hello', reset: 'power-on' }));
    ws.send(JSON.stringify({ type: 'ping' }));
  };

  const openSecondDevice = () => {
    ws2 = new WebSocket('ws://127.0.0.1:18765');
    ws2.on('open', () => {
      ws2.send(JSON.stringify({ type: 'dev.hello', reset: 'power-on' }));
      ws2.send(JSON.stringify({ type: 'ping' }));
    });
    ws2.on('message', (d) => {
      let m;
      try {
        m = JSON.parse(d.toString());
      } catch {
        return fail(`garbage frame: ${d.toString().slice(0, 60)}`);
      }
      if (m.type === 'pong') { dup.pong2 = true; dup.finish(); }
    });
    ws2.on('close', (code) => fail(`new device socket wrongly closed (${code})`));
    ws2.on('error', (e) => fail(e.message));
  };

  ws.on('open', () => ws.send(JSON.stringify({ type: 'ping' })));
  ws.on('close', (code) => {
    if (code === 4000 && !dup.oldClosed) {
      dup.oldClosed = true;
      dup.finish();
    }
  });
  ws.on('error', (e) => fail(e.message));
  let pongs = 0;
  ws.on('message', (d) => {
    let m;
    try {
      m = JSON.parse(d.toString());
    } catch {
      return fail(`garbage frame: ${d.toString().slice(0, 60)}`);
    }
    if (m.type === 'hello') got.hello = true;
    if (m.type === 'chat') got.chat = true;
    if (m.type === 'pong') {
      pongs += 1;
      if (pongs === 1) {
        got.pong = true;
      } else if (pongs === 2 && dup.started) {
        openSecondDevice();
      }
    }
    if (got.hello && got.pong && got.chat) {
      clearTimeout(timer);
      startDupCheck();
    }
  });
}, 1500);
