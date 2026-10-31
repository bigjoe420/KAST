'use strict';

/* KAST motion graphics — overlay mode (owner-approved 2026-10-31).
 *
 * A hidden "KAST GFX" scene holds one media source per graphic. Like
 * "KAST SFX" it is nested as a scene item into whatever scene is live, so
 * the graphic rides ON TOP of the current layout — the program scene never
 * changes. A tap enables the item, restarts the clip, and a timer hides it
 * again after the graphic's configured duration (default 5 s). An optional
 * bound sound fires through the SFX path in the same breath (server.js
 * owns that pairing; this class only shows the graphic).
 *
 * KAST creates and removes only its exact managed names; user scenes,
 * sources, and scene items are never touched. */

const SCENE_NAME = 'KAST GFX';
const SOURCE_PREFIX = 'KAST GFX - ';
const RESTART = 'OBS_WEBSOCKET_MEDIA_INPUT_ACTION_RESTART';
const REQUIRED_REQUESTS = [
  'GetVersion', 'GetSceneList', 'CreateScene', 'GetInputList', 'CreateInput',
  'GetCurrentProgramScene', 'GetSceneItemId', 'CreateSceneItem',
  'SetSceneItemEnabled', 'TriggerMediaInputAction', 'RemoveInput', 'RemoveScene',
];

function sourceName(id) {
  return (SOURCE_PREFIX + String(id)).slice(0, 64);
}

class ObsGfx {
  constructor({ client, listGraphics, bindingFor, logger = console } = {}) {
    this.client = client;
    this.listGraphics = listGraphics;
    this.bindingFor = bindingFor || (() => ({ sound: null, durationMs: 5000 }));
    this.logger = logger;
    this.setupPromise = null;
    this.hideTimers = new Map();   /* id -> hide timeout */
  }

  async checkCapabilities() {
    const version = await this.client.request('GetVersion');
    const available = new Set(version.availableRequests || []);
    const missing = REQUIRED_REQUESTS.filter((name) => !available.has(name));
    if (missing.length) {
      throw new Error(`OBS does not support required OBS requests: ${missing.join(', ')}`);
    }
    return version;
  }

  setup() {
    if (this.setupPromise) return this.setupPromise;
    this.setupPromise = this.performSetup().finally(() => { this.setupPromise = null; });
    return this.setupPromise;
  }

  async performSetup() {
    await this.checkCapabilities();
    const scenes = await this.client.request('GetSceneList');
    if (!scenes.scenes.some((scene) => scene.sceneName === SCENE_NAME)) {
      await this.client.request('CreateScene', { sceneName: SCENE_NAME });
    }
    const existing = new Set((await this.client.request('GetInputList')).inputs.map((x) => x.inputName));
    const desired = new Set(this.listGraphics().map((gfx) => sourceName(gfx.id)));
    for (const inputName of existing) {
      if (inputName.startsWith(SOURCE_PREFIX) && !desired.has(inputName)) {
        await this.client.request('RemoveInput', { inputName });
        existing.delete(inputName);
      }
    }
    for (const gfx of this.listGraphics()) {
      const inputName = sourceName(gfx.id);
      if (!existing.has(inputName)) {
        await this.client.request('CreateInput', {
          sceneName: SCENE_NAME,
          inputName,
          inputKind: 'ffmpeg_source',
          inputSettings: {
            local_file: gfx.file,
            is_local_file: true,
            looping: false,
            restart_on_activate: false,
          },
          /* overlays start hidden; play() reveals them on demand */
          sceneItemEnabled: false,
        });
      }
    }
    await this.linkActiveScene();
  }

  /* Nest KAST GFX into the live scene so overlays show on top of whatever
     is on air. Follows the user across scene switches (server calls this
     again on CurrentProgramSceneChanged). */
  async linkActiveScene() {
    const current = await this.client.request('GetCurrentProgramScene');
    const sceneName = current.sceneName || current.currentProgramSceneName;
    if (!sceneName || sceneName === SCENE_NAME) return;
    try {
      await this.client.request('GetSceneItemId', { sceneName, sourceName: SCENE_NAME });
    } catch (err) {
      if (err.code !== 600) throw err;
      await this.client.request('CreateSceneItem', { sceneName, sourceName: SCENE_NAME, sceneItemEnabled: true });
    }
  }

  async play(id) {
    const gfx = this.listGraphics().find((entry) => entry.id === id);
    if (!gfx) throw new Error(`Graphic '${id}' is not in the graphics catalog`);
    await this.setup();
    await this.linkActiveScene();
    const inputName = sourceName(id);
    const { sceneItemId } = await this.client.request('GetSceneItemId', {
      sceneName: SCENE_NAME, sourceName: inputName,
    });
    await this.client.request('SetSceneItemEnabled', {
      sceneName: SCENE_NAME, sceneItemId, sceneItemEnabled: true,
    });
    await this.client.request('TriggerMediaInputAction', { inputName, mediaAction: RESTART });
    this.scheduleHide(id, sceneItemId);
    return gfx;
  }

  scheduleHide(id, sceneItemId) {
    clearTimeout(this.hideTimers.get(id));
    const { durationMs } = this.bindingFor(id);
    this.hideTimers.set(id, setTimeout(() => {
      this.hideTimers.delete(id);
      this.client.request('SetSceneItemEnabled', {
        sceneName: SCENE_NAME, sceneItemId, sceneItemEnabled: false,
      }).catch((err) => this.logger.log(`[obs] gfx hide '${id}' failed: ${err.message}`));
    }, durationMs));
  }

  /* Detach one managed source before its file is replaced/removed (OBS
     locks media files on Windows — same discipline as ObsSfx.release). */
  async release(id) {
    clearTimeout(this.hideTimers.get(id));
    this.hideTimers.delete(id);
    const inputName = sourceName(id);
    const inputs = await this.client.request('GetInputList');
    if (inputs.inputs.some((input) => input.inputName === inputName)) {
      await this.client.request('RemoveInput', { inputName });
    }
  }

  async remove() {
    for (const timer of this.hideTimers.values()) clearTimeout(timer);
    this.hideTimers.clear();
    const inputs = await this.client.request('GetInputList');
    for (const input of inputs.inputs) {
      if (input.inputName.startsWith(SOURCE_PREFIX)) {
        await this.client.request('RemoveInput', { inputName: input.inputName });
      }
    }
    const scenes = await this.client.request('GetSceneList');
    if (scenes.scenes.some((scene) => scene.sceneName === SCENE_NAME)) {
      await this.client.request('RemoveScene', { sceneName: SCENE_NAME });
    }
  }
}

module.exports = { ObsGfx, sourceName, SCENE_NAME, SOURCE_PREFIX };
