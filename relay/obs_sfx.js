'use strict';

const SCENE_NAME = 'KAST SFX';
const SOURCE_PREFIX = 'KAST SFX - ';
const MONITOR_AND_OUTPUT = 'OBS_MONITORING_TYPE_MONITOR_AND_OUTPUT';
const RESTART = 'OBS_WEBSOCKET_MEDIA_INPUT_ACTION_RESTART';
const REQUIRED_REQUESTS = [
  'GetVersion', 'GetSceneList', 'CreateScene', 'GetInputList', 'CreateInput',
  'SetInputAudioMonitorType', 'SetInputVolume', 'GetCurrentProgramScene',
  'GetSceneItemId', 'CreateSceneItem', 'TriggerMediaInputAction',
  'RemoveInput', 'RemoveScene',
];

function sourceName(id) {
  return (SOURCE_PREFIX + String(id)).slice(0, 64);
}

class ObsSfx {
  constructor({ client, listSounds, volume = 1, logger = console } = {}) {
    this.client = client;
    this.listSounds = listSounds;
    this.volume = volume;
    this.logger = logger;
    this.setupPromise = null;
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
    const desired = new Set(this.listSounds().map((sound) => sourceName(sound.id)));
    for (const inputName of existing) {
      if (inputName.startsWith(SOURCE_PREFIX) && !desired.has(inputName)) {
        await this.client.request('RemoveInput', { inputName });
        existing.delete(inputName);
      }
    }
    for (const sound of this.listSounds()) {
      const inputName = sourceName(sound.id);
      if (!existing.has(inputName)) {
        await this.client.request('CreateInput', {
          sceneName: SCENE_NAME,
          inputName,
          inputKind: 'ffmpeg_source',
          inputSettings: {
            local_file: sound.file,
            is_local_file: true,
            looping: false,
            restart_on_activate: true,
          },
          sceneItemEnabled: true,
        });
      }
      await this.client.request('SetInputAudioMonitorType', { inputName, monitorType: MONITOR_AND_OUTPUT });
      await this.client.request('SetInputVolume', { inputName, inputVolumeMul: this.volume });
    }
    await this.linkActiveScene();
  }

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
    const sound = this.listSounds().find((entry) => entry.id === id);
    if (!sound) throw new Error(`Sound '${id}' is not in the sound catalog`);
    await this.setup();
    await this.client.request('TriggerMediaInputAction', { inputName: sourceName(id), mediaAction: RESTART });
    return sound;
  }

  async setVolume(value) {
    const volume = Number(value);
    if (!Number.isFinite(volume) || volume < 0 || volume > 2) {
      throw new Error('SFX volume must be between 0 and 2');
    }
    this.volume = volume;
    const inputs = await this.client.request('GetInputList');
    for (const input of inputs.inputs) {
      if (input.inputName.startsWith(SOURCE_PREFIX)) {
        await this.client.request('SetInputVolume', { inputName: input.inputName, inputVolumeMul: volume });
      }
    }
  }

  async release(id) {
    const inputName = sourceName(id);
    const inputs = await this.client.request('GetInputList');
    if (inputs.inputs.some((input) => input.inputName === inputName)) {
      await this.client.request('RemoveInput', { inputName });
    }
  }

  async remove() {
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

module.exports = { ObsSfx, sourceName, SCENE_NAME, SOURCE_PREFIX };
