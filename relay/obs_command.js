#!/usr/bin/env node
'use strict';

/* Companion's small local command channel. The relay remains the only
   process connected to OBS; this client just asks it to run one action. */
const WebSocket = require('ws');

const action = process.argv[2];
const value = process.argv[3];
const port = Number(process.env.KAST_RELAY_PORT || 8765);
if (!['setup', 'test', 'remove', 'release', 'volume', 'gfxrelease', 'gfxplay'].includes(action)) {
  console.error('Usage: node obs_command.js <setup|test|remove|release|volume|gfxrelease|gfxplay> [value]');
  process.exit(2);
}

const ws = new WebSocket(`ws://127.0.0.1:${port}`);
const timer = setTimeout(() => {
  console.error('KAST relay did not answer the OBS command.');
  ws.terminate();
  process.exit(1);
}, 15000);

ws.on('open', () => {
  const argument = value === undefined ? {} : (action === 'volume' ? { value: Number(value) } : { id: value });
  ws.send(JSON.stringify({ type: 'obs.command', action, ...argument }));
});
ws.on('message', (raw) => {
  let msg;
  try { msg = JSON.parse(raw.toString()); } catch { return; }
  if (msg.type !== 'obs.ack' || msg.action !== action) return;
  clearTimeout(timer);
  if (msg.ok) {
    console.log(action === 'test' ? 'Test sound sent through OBS.' : `OBS ${action} completed.`);
    ws.close();
    process.exit(0);
  }
  console.error(msg.error || 'OBS command failed.');
  ws.close();
  process.exit(1);
});
ws.on('error', (err) => {
  clearTimeout(timer);
  console.error(`Cannot reach KAST relay: ${err.message}`);
  process.exit(1);
});
