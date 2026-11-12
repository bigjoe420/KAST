'use strict';

/* KAST PANIC button (owner-approved 2026-11-01): one tap on the box and
 * the stream is safe — the mic input mutes AND a full-screen cover drops
 * over whatever is on air (bad sounds, bad words, anything that should
 * not stay live). A second tap lifts the cover and unmutes.
 *
 * The cover is a managed "KAST PANIC" scene holding one full-screen
 * color source, nested into the active scene exactly like KAST GFX /
 * KAST SFX — the program scene never changes. The mic is the OBS special
 * input "mic1" unless the owner pins a different name (KAST_PANIC_MIC).
 *
 * KAST creates and removes only its exact managed names; user scenes,
 * sources, and scene items are never touched. */

const SCENE_NAME = 'KAST PANIC';
const COVER_NAME = 'KAST PANIC COVER';
const REQUIRED_REQUESTS = [
  'GetVersion', 'GetSceneList', 'CreateScene', 'GetInputList', 'CreateInput',
  'GetCurrentProgramScene', 'GetSceneItemId', 'CreateSceneItem',
  'SetSceneItemEnabled', 'RemoveInput', 'RemoveScene',
  'GetSpecialInputs', 'SetInputMute',
];

class ObsPanic {
  constructor({ client, micName = '', logger = console } = {}) {
    this.client = client;
    this.micName = micName;      /* '' = ask OBS for its mic1 special input */
    this.logger = logger;
    this.active = false;
    this.setupPromise = null;
  }

  async checkCapabilities() {
    const version = await this.client.request('GetVersion');
    const available = new Set(version.availableRequests || []);
    const missing = REQUIRED_REQUESTS.filter((name) => !available.has(name));
    if (missing.length) {
      throw new Error(`OBS does not support required OBS requests: ${missing.join(', ')}`);
    }
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
    const inputs = new Set((await this.client.request('GetInputList')).inputs.map((x) => x.inputName));
    if (!inputs.has(COVER_NAME)) {
      await this.client.request('CreateInput', {
        sceneName: SCENE_NAME,
        inputName: COVER_NAME,
        inputKind: 'color_source_v3',
        inputSettings: {
          color: 0xFF000000,     /* opaque black — covers anything under it */
          width: 1920,
          height: 1080,
        },
        sceneItemEnabled: true,
      });
    }
  }

  /* Nest KAST PANIC into the live scene so the cover rides on top. Only
     needed while panic is active; follows scene switches like the other
     overlay scenes (server calls this on CurrentProgramSceneChanged). */
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

  async resolveMic() {
    if (this.micName) return this.micName;
    const special = await this.client.request('GetSpecialInputs');
    return special.mic1 || null;
  }

  async setMicMuted(muted) {
    const mic = await this.resolveMic();
    if (!mic) {
      this.logger.log('[obs] panic: no mic input found — cover only');
      return;
    }
    await this.client.request('SetInputMute', { inputName: mic, inputMuted: muted });
    this.logger.log(`[obs] panic: mic '${mic}' ${muted ? 'MUTED' : 'unmuted'}`);
  }

  /* Returns the resulting state (true = stream covered + mic muted). */
  async set(on) {
    if (on === this.active) return this.active;
    await this.setup();
    if (on) {
      /* cover AND mic mute fire in parallel — on a panic tap there is no
         "first one, then the other", everything lands in the same beat */
      await Promise.all([this.linkActiveScene(), this.setMicMuted(true)]);
    } else {
      await this.setMicMuted(false);
      const scenes = await this.client.request('GetSceneList');
      for (const { sceneName } of scenes.scenes) {
        if (sceneName === SCENE_NAME) continue;
        try {
          const { sceneItemId } = await this.client.request('GetSceneItemId', {
            sceneName, sourceName: SCENE_NAME,
          });
          await this.client.request('SetSceneItemEnabled', {
            sceneName, sceneItemId, sceneItemEnabled: false,
          });
        } catch (err) {
          if (err.code !== 600) {
            this.logger.log(`[obs] panic cover lift on '${sceneName}' failed: ${err.message}`);
          }
        }
      }
    }
    this.active = on;
    return this.active;
  }

  async toggle() {
    return this.set(!this.active);
  }

  /* KAST-only cleanup (Companion "Remove KAST setup" path). */
  async remove() {
    if (this.active) {
      await this.setMicMuted(false).catch(() => {});
      this.active = false;
    }
    const inputs = await this.client.request('GetInputList');
    if (inputs.inputs.some((input) => input.inputName === COVER_NAME)) {
      await this.client.request('RemoveInput', { inputName: COVER_NAME });
    }
    const scenes = await this.client.request('GetSceneList');
    if (scenes.scenes.some((scene) => scene.sceneName === SCENE_NAME)) {
      await this.client.request('RemoveScene', { sceneName: SCENE_NAME });
    }
  }
}

module.exports = { ObsPanic, SCENE_NAME, COVER_NAME };
