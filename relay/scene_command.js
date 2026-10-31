#!/usr/bin/env node
'use strict';

/* Scene tile management for the Companion picker.
 *   list            -> asks the running relay for OBS's user scenes plus the
 *                      current selection, prints JSON {scenes, selected}
 *   set <manifest>  -> writes the selection file locally, then asks the
 *                      relay to rebroadcast the box tile catalog
 * The relay remains the only process connected to OBS. */

const fs = require('fs');
const path = require('path');
const WebSocket = require('ws');
const { ObsScenes } = require('./obs_scenes');

const selectionPath = path.join(__dirname, 'scene_tiles.json');
const port = Number(process.env.KAST_RELAY_PORT || 8765);
const [action, manifest] = process.argv.slice(2);

function askRelay(frame, replyType) {
  return new Promise((resolve, reject) => {
    const ws = new WebSocket(`ws://127.0.0.1:${port}`);
    const timer = setTimeout(() => {
      ws.terminate();
      reject(new Error('KAST relay did not answer.'));
    }, 15000);
    ws.on('open', () => ws.send(JSON.stringify(frame)));
    ws.on('message', (raw) => {
      let msg;
      try { msg = JSON.parse(raw.toString()); } catch { return; }
      if (msg.type !== replyType) return;
      clearTimeout(timer);
      ws.close();
      if (msg.ok === false) reject(new Error(msg.error || 'Relay command failed.'));
      else resolve(msg);
    });
    ws.on('error', (err) => {
      clearTimeout(timer);
      reject(new Error(`Cannot reach KAST relay: ${err.message}`));
    });
  });
}

(async () => {
  if (action === 'list') {
    const reply = await askRelay({ type: 'scenes.list.req' }, 'scenes.list');
    console.log(JSON.stringify({ scenes: reply.scenes, selected: reply.selected }));
  } else if (action === 'set') {
    /* Windows PowerShell 5.1 writes UTF-8 with a BOM — strip one leading BOM */
    const names = JSON.parse(String(fs.readFileSync(manifest, 'utf8')).replace(/^\uFEFF/, ''));
    const scenes = new ObsScenes({ client: null, selectionPath });
    const saved = scenes.writeSelection(names);
    await askRelay({ type: 'scenes.refresh' }, 'scenes.refreshed');
    console.log(`Tiles: ${saved.length ? saved.join(', ') : '(none)'}`);
  } else {
    throw new Error('Usage: scene_command.js <list|set> [manifest]');
  }
})().catch((err) => {
  console.error(err.message);
  process.exit(1);
});
