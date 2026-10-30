'use strict';

const test = require('node:test');
const assert = require('node:assert/strict');
const { ObsSfx, sourceName } = require('./obs_sfx');

const capabilities = [
  'GetVersion', 'GetSceneList', 'CreateScene', 'GetInputList', 'CreateInput',
  'SetInputAudioMonitorType', 'SetInputVolume', 'GetCurrentProgramScene',
  'GetSceneItemId', 'CreateSceneItem', 'TriggerMediaInputAction',
  'RemoveInput', 'RemoveScene',
];

class FakeObs {
  constructor() {
    this.scenes = ['Main', 'Other'];
    this.current = 'Main';
    this.inputs = [];
    this.links = new Set();
    this.calls = [];
    this.restarts = [];
  }

  async request(type, data = {}) {
    this.calls.push({ type, data });
    const ok = (responseData = {}) => responseData;
    if (type === 'GetVersion') return ok({ availableRequests: capabilities });
    if (type === 'GetSceneList') return ok({ scenes: this.scenes.map((sceneName) => ({ sceneName })) });
    if (type === 'CreateScene') { this.scenes.push(data.sceneName); return ok(); }
    if (type === 'GetInputList') return ok({ inputs: this.inputs.map((inputName) => ({ inputName })) });
    if (type === 'CreateInput') { this.inputs.push(data.inputName); return ok(); }
    if (type === 'GetCurrentProgramScene') return ok({ sceneName: this.current });
    if (type === 'GetSceneItemId') {
      if (!this.links.has(`${data.sceneName}/${data.sourceName}`)) {
        const err = new Error('not found'); err.code = 600; throw err;
      }
      return ok({ sceneItemId: 1 });
    }
    if (type === 'CreateSceneItem') { this.links.add(`${data.sceneName}/${data.sourceName}`); return ok(); }
    if (type === 'TriggerMediaInputAction') { this.restarts.push(data.inputName); return ok(); }
    if (type === 'RemoveInput') { this.inputs = this.inputs.filter((x) => x !== data.inputName); return ok(); }
    if (type === 'RemoveScene') { this.scenes = this.scenes.filter((x) => x !== data.sceneName); return ok(); }
    return ok();
  }
}

const sounds = [{ id: 'applause', file: 'C:\\media\\applause.mp3' }];

test('setup creates only managed items with monitor and output enabled', async () => {
  const client = new FakeObs();
  const sfx = new ObsSfx({ client, listSounds: () => sounds });
  await sfx.setup();
  assert.deepEqual(client.scenes, ['Main', 'Other', 'KAST SFX']);
  assert.deepEqual(client.inputs, ['KAST SFX - applause']);
  const create = client.calls.find((c) => c.type === 'CreateInput');
  assert.equal(create.data.inputSettings.local_file, sounds[0].file);
  assert.equal(create.data.inputKind, 'ffmpeg_source');
  const monitor = client.calls.find((c) => c.type === 'SetInputAudioMonitorType');
  assert.equal(monitor.data.monitorType, 'OBS_MONITORING_TYPE_MONITOR_AND_OUTPUT');
  assert(client.links.has('Main/KAST SFX'));
});

test('repeated setup and taps do not duplicate sources or scene links', async () => {
  const client = new FakeObs();
  const sfx = new ObsSfx({ client, listSounds: () => sounds });
  await Promise.all([sfx.setup(), sfx.setup()]);
  await sfx.play('applause');
  client.current = 'Other';
  await sfx.play('applause');
  assert.deepEqual(client.inputs, ['KAST SFX - applause']);
  assert.equal(client.calls.filter((c) => c.type === 'CreateSceneItem').length, 2);
  assert.deepEqual(client.restarts, ['KAST SFX - applause', 'KAST SFX - applause']);
});

test('playing an unknown catalog id fails without calling OBS', async () => {
  const client = new FakeObs();
  const sfx = new ObsSfx({ client, listSounds: () => sounds });
  await assert.rejects(sfx.play('missing'), /not in the sound catalog/);
  assert.equal(client.calls.length, 0);
});

test('setup reconciles stale managed sources without touching user inputs', async () => {
  const client = new FakeObs();
  client.inputs = ['KAST SFX - old', 'My Music'];
  const sfx = new ObsSfx({ client, listSounds: () => sounds });
  await sfx.setup();
  assert.deepEqual(client.inputs, ['My Music', 'KAST SFX - applause']);
});

test('cleanup removes only exact KAST-owned inputs and scene', async () => {
  const client = new FakeObs();
  client.inputs = ['KAST SFX - applause', 'KAST SFX backup', 'My Music'];
  const sfx = new ObsSfx({ client, listSounds: () => sounds });
  await sfx.remove();
  assert.deepEqual(client.inputs, ['KAST SFX backup', 'My Music']);
  assert.deepEqual(client.scenes, ['Main', 'Other']);
  assert(!client.calls.some((c) => c.type === 'RemoveInput' && c.data.inputName === 'My Music'));
});

test('missing required OBS capabilities produce a clear setup error', async () => {
  const client = new FakeObs();
  const original = client.request.bind(client);
  client.request = async (type, data) => {
    if (type === 'GetVersion') return { availableRequests: ['GetVersion'] };
    return original(type, data);
  };
  const sfx = new ObsSfx({ client, listSounds: () => sounds });
  await assert.rejects(sfx.setup(), /does not support required OBS requests/);
  assert(!client.scenes.includes('KAST SFX'));
});

test('source names are bounded and cannot imitate the managed scene', () => {
  assert.equal(sourceName('applause'), 'KAST SFX - applause');
  assert(sourceName('x'.repeat(100)).length <= 64);
});
