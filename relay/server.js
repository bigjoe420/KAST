#!/usr/bin/env node
/*
 * KAST PC relay — local bridge between the ESP32 terminal and the outside
 * world (Kick chat now, OBS in Phase 4). Runs on the streaming PC.
 *
 * Today (skeleton):
 *   - accepts WebSocket connections from the device
 *   - speaks a small JSON line protocol (hello / ping-pong / chat frames)
 *   - --simulate-chat: bench mode, pushes a fake chat line every 5 s so
 *     the device receive path can be verified without Kick credentials
 *
 * Next: Kick API auth (read/send chat). Credentials live in relay/.env
 * (gitignored) and never on the device.
 *
 * Usage: node server.js [--simulate-chat]
 */
'use strict';

const fs = require('fs');
const path = require('path');
const { WebSocketServer } = require('ws');

/* Config: relay/.env (gitignored) overrides the process environment. */
const env_path = path.join(__dirname, '.env');
if (fs.existsSync(env_path)) {
  for (const line of fs.readFileSync(env_path, 'utf8').split(/\r?\n/)) {
    const m = line.match(/^\s*([A-Z0-9_]+)\s*=\s*(.+?)\s*$/);
    if (m && !process.env[m[1]]) {
      process.env[m[1]] = m[2];
    }
  }
}

const PORT = Number(process.env.KAST_RELAY_PORT || 8765);
const SIMULATE = process.argv.includes('--simulate-chat');

const wss = new WebSocketServer({ port: PORT });
const clients = new Set();

function broadcast(obj) {
  const line = JSON.stringify(obj);
  for (const ws of clients) {
    if (ws.readyState === ws.OPEN) {
      ws.send(line);
    }
  }
}

wss.on('connection', (ws, req) => {
  clients.add(ws);
  console.log(`[relay] device connected (${req.socket.remoteAddress}), ${clients.size} client(s)`);
  if (ws.readyState === ws.OPEN) {
    ws.send(JSON.stringify({ type: 'hello', app: 'kast-relay', version: '0.1.0' }));
  }

  ws.on('message', (data) => {
    let msg;
    try {
      msg = JSON.parse(data.toString());
    } catch {
      console.log(`[relay] non-JSON frame dropped: ${data.toString().slice(0, 80)}`);
      return;
    }
    switch (msg.type) {
      case 'ping':
        ws.send(JSON.stringify({ type: 'pong', t: Date.now() }));
        break;
      case 'chat.send':
        /* Phase 3: inject into Kick via the API using .env credentials. */
        console.log(`[relay] chat.send (Kick not wired yet): ${msg.text}`);
        break;
      default:
        console.log(`[relay] rx: ${data.toString().slice(0, 120)}`);
    }
  });

  ws.on('close', () => {
    clients.delete(ws);
    console.log(`[relay] device disconnected, ${clients.size} client(s)`);
  });
  ws.on('error', (err) => console.log(`[relay] socket error: ${err.message}`));
});

if (SIMULATE) {
  const names = ['benchbot', 'glass_check', 'night_owl', 'streamfan'];
  const lines = [
    'relay link is alive',
    'typing from the bench test',
    'the K towers are spinning',
    'if you can read this, the path works',
  ];
  setInterval(() => {
    if (clients.size === 0) {
      return;
    }
    broadcast({
      type: 'chat',
      user: names[Math.floor(Math.random() * names.length)],
      text: lines[Math.floor(Math.random() * lines.length)],
      ts: Date.now(),
    });
  }, 5000);
  console.log('[relay] chat simulator on (5s interval)');
}

console.log(`[relay] KAST relay listening on ws://0.0.0.0:${PORT}`);
process.on('SIGINT', () => {
  console.log('\n[relay] bye');
  process.exit(0);
});
