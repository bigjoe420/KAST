/* One-shot KAST-only OBS teardown — fresh-install rehearsal prep.
 * Lifts a leftover PANIC cover (and unmutes the mic if KAST muted it),
 * then removes every KAST-managed scene and source. User scenes and
 * settings are never touched. Run from relay/: node obs_teardown.js */
'use strict';

const fs = require('fs');
const path = require('path');

const envPath = path.join(__dirname, '.env');
if (fs.existsSync(envPath)) {
  for (const line of fs.readFileSync(envPath, 'utf8').split(/\r?\n/)) {
    const m = line.match(/^\s*([A-Z0-9_]+)\s*=\s*(.*)\s*$/);
    if (m && !process.env[m[1]]) process.env[m[1]] = m[2];
  }
}

const { ObsClient } = require('./obs');
const { ObsSfx } = require('./obs_sfx');
const { ObsGfx } = require('./obs_gfx');
const { ObsPanic, SCENE_NAME: PANIC_SCENE } = require('./obs_panic');

const log = (s) => console.log(`[teardown] ${s}`);

async function main() {
  const client = new ObsClient({
    url: process.env.KAST_OBS_URL || 'ws://127.0.0.1:4455',
    password: process.env.KAST_OBS_PASSWORD || '',
    reconnect: false,
  });
  await client.connect();
  log('connected to OBS');

  /* raw probe: ask OBS to drop one managed input and show the FULL response
     so we see whether requestStatus.result is really true */
  const probe = (await client.request('GetInputList')).inputs
    .map((i) => i.inputName).filter((n) => n.startsWith('KAST')).slice(0, 3);
  for (const name of probe) {
    const r = await client.request('RemoveInput', { inputName: name });
    log(`probe RemoveInput '${name}': ${JSON.stringify(r && r.requestStatus ? r.requestStatus : r)}`);
  }

  /* 1 — lift any leftover PANIC cover in every user scene */
  const scenes = await client.request('GetSceneList');
  let engaged = 0;
  for (const { sceneName } of scenes.scenes) {
    if (sceneName === PANIC_SCENE) continue;
    try {
      const { sceneItemId } = await client.request('GetSceneItemId', {
        sceneName, sourceName: PANIC_SCENE,
      });
      const { sceneItemEnabled } = await client.request('GetSceneItemEnabled', {
        sceneName, sceneItemId,
      });
      if (sceneItemEnabled) engaged += 1;
      await client.request('SetSceneItemEnabled', {
        sceneName, sceneItemId, sceneItemEnabled: false,
      });
    } catch (err) {
      if (err.code !== 600) log(`cover lift on '${sceneName}' failed: ${err.message}`);
    }
  }
  log(`panic cover lifted from ${engaged} scene(s)`);

  /* 2 — if KAST had the stream covered, the mic was muted by KAST: restore it */
  if (engaged > 0) {
    const special = await client.request('GetSpecialInputs');
    const micName = process.env.KAST_PANIC_MIC || special.mic1;
    if (micName) {
      const { inputMuted } = await client.request('GetInputMute', { inputName: micName });
      if (inputMuted) {
        await client.request('SetInputMute', { inputName: micName, inputMuted: false });
        log(`mic '${micName}' unmuted`);
      }
    }
  }

  /* 3 — remove all KAST-managed scenes/sources via the modules' own cleanup */
  const noop = async () => [];
  await new ObsSfx({ client, listSounds: noop }).remove();
  log('sfx scene/sources removed');
  await new ObsGfx({ client, listGraphics: noop }).remove();
  log('gfx scene/sources removed');
  await new ObsPanic({ client }).remove();
  log('panic scene/sources removed');

  /* 4 — verify nothing KAST-managed remains */
  const leftScenes = (await client.request('GetSceneList')).scenes
    .map((s) => s.sceneName).filter((n) => n.startsWith('KAST'));
  const leftInputs = (await client.request('GetInputList')).inputs
    .map((i) => i.inputName).filter((n) => n.startsWith('KAST'));
  log(`verification — KAST scenes left: [${leftScenes}] inputs left: [${leftInputs}]`);

  client.close && client.close();
  process.exit(leftScenes.length + leftInputs.length);
}

main().catch((err) => { console.error(`[teardown] FAILED: ${err.message}`); process.exit(1); });
