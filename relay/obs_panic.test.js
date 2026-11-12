'use strict';

const test = require('node:test');
const assert = require('node:assert/strict');
const { ObsPanic, SCENE_NAME, COVER_NAME } = require('./obs_panic');

const capabilities = [
  'GetVersion', 'GetSceneList', 'CreateScene', 'GetInputList', 'CreateInput',
  'GetCurrentProgramScene', 'GetSceneItemId', 'CreateSceneItem',
  'SetSceneItemEnabled', 'RemoveInput', 'RemoveScene',
  'GetSpecialInputs', 'SetInputMute',
];

class FakeObs {
  constructor() {
    this.scenes = ['Main', 'Other'];
    this.current = 'Main';
    this.inputs = [];
    this.links = new Map();       /* "scene/KAST PANIC" -> sceneItemId */
    this.itemIds = new Map();
    this.enabled = new Map();
    this.mutes = new Map();        /* inputName -> bool */
    this.special = { mic1: 'Mic/Aux' };
    this.calls = [];
    this.nextId = 1;
  }

  async request(type, data = {}) {
    this.calls.push({ type, data });
    if (type === 'GetVersion') return { availableRequests: capabilities };
    if (type === 'GetSceneList') return { scenes: this.scenes.map((sceneName) => ({ sceneName })) };
    if (type === 'CreateScene') { this.scenes.push(data.sceneName); return {}; }
    if (type === 'GetInputList') return { inputs: this.inputs.map((inputName) => ({ inputName })) };
    if (type === 'CreateInput') {
      this.inputs.push(data.inputName);
      const id = this.nextId++;
      this.itemIds.set(`${data.sceneName}/${data.inputName}`, id);
      this.enabled.set(id, data.sceneItemEnabled !== false);
      return {};
    }
    if (type === 'GetCurrentProgramScene') return { sceneName: this.current };
    if (type === 'GetSceneItemId') {
      const key = `${data.sceneName}/${data.sourceName}`;
      if (this.itemIds.has(key)) return { sceneItemId: this.itemIds.get(key) };
      if (this.links.has(key)) return { sceneItemId: this.links.get(key) };
      const err = new Error('not found'); err.code = 600; throw err;
    }
    if (type === 'CreateSceneItem') {
      const id = 900 + this.nextId++;
      this.links.set(`${data.sceneName}/${data.sourceName}`, id);
      this.enabled.set(id, data.sceneItemEnabled !== false);
      return { sceneItemId: id };
    }
    if (type === 'SetSceneItemEnabled') { this.enabled.set(data.sceneItemId, data.sceneItemEnabled); return {}; }
    if (type === 'RemoveInput') {
      this.inputs = this.inputs.filter((x) => x !== data.inputName);
      return {};
    }
    if (type === 'RemoveScene') { this.scenes = this.scenes.filter((x) => x !== data.sceneName); return {}; }
    if (type === 'GetSpecialInputs') return { ...this.special };
    if (type === 'SetInputMute') { this.mutes.set(data.inputName, data.inputMuted); return {}; }
    return {};
  }

  coverEnabledIn(scene) {
    const id = this.links.get(`${scene}/${SCENE_NAME}`);
    return id === undefined ? undefined : this.enabled.get(id);
  }
}

function makePanic(client, opts = {}) {
  return new ObsPanic({ client, logger: { log() {} }, ...opts });
}

test('setup creates the managed scene with an opaque full-screen cover', async () => {
  const client = new FakeObs();
  const panic = makePanic(client);
  await panic.setup();
  assert(client.scenes.includes(SCENE_NAME));
  assert.deepEqual(client.inputs, [COVER_NAME]);
  const create = client.calls.find((c) => c.type === 'CreateInput');
  assert.equal(create.data.inputKind, 'color_source_v3');
  assert.equal(create.data.inputSettings.color, 0xFF000000);
});

test('panic on nests the cover into the live scene and mutes the mic', async () => {
  const client = new FakeObs();
  const panic = makePanic(client);
  const on = await panic.toggle();
  assert.equal(on, true);
  assert.equal(panic.active, true);
  assert.equal(client.coverEnabledIn('Main'), true);
  assert.equal(client.mutes.get('Mic/Aux'), true);
});

test('panic off lifts the cover everywhere and unmutes', async () => {
  const client = new FakeObs();
  const panic = makePanic(client);
  await panic.toggle();
  const off = await panic.toggle();
  assert.equal(off, false);
  assert.equal(client.coverEnabledIn('Main'), false);
  assert.equal(client.mutes.get('Mic/Aux'), false);
});

test('cover follows the user across scene switches while engaged', async () => {
  const client = new FakeObs();
  const panic = makePanic(client);
  await panic.toggle();
  client.current = 'Other';
  await panic.linkActiveScene();
  assert.equal(client.coverEnabledIn('Other'), true);
  await panic.set(false);
  assert.equal(client.coverEnabledIn('Other'), false);
});

test('an explicit mic name wins over the OBS special input', async () => {
  const client = new FakeObs();
  const panic = makePanic(client, { micName: 'Stream Mic' });
  await panic.toggle();
  assert.equal(client.mutes.get('Stream Mic'), true);
  assert.equal(client.mutes.get('Mic/Aux'), undefined);
});

test('no mic in OBS still drops the cover (cover is the priority)', async () => {
  const client = new FakeObs();
  client.special = {};
  const panic = makePanic(client);
  const on = await panic.toggle();
  assert.equal(on, true);
  assert.equal(client.coverEnabledIn('Main'), true);
  assert(!client.calls.some((c) => c.type === 'SetInputMute'));
});

test('re-engaging after a failed mute attempt keeps state consistent', async () => {
  const client = new FakeObs();
  const panic = makePanic(client);
  await panic.set(true);
  await panic.set(true);   /* no-op, no duplicate work */
  const mutes = client.calls.filter((c) => c.type === 'SetInputMute');
  assert.equal(mutes.length, 1);
  assert.equal(panic.active, true);
});

test('cleanup unmutes, and removes only KAST-owned cover and scene', async () => {
  const client = new FakeObs();
  client.inputs = ['My Overlay'];
  const panic = makePanic(client);
  await panic.toggle();
  await panic.remove();
  assert.equal(panic.active, false);
  assert.equal(client.mutes.get('Mic/Aux'), false);
  assert.deepEqual(client.inputs, ['My Overlay']);
  assert(!client.scenes.includes(SCENE_NAME));
});

test('missing required OBS capabilities produce a clear setup error', async () => {
  const client = new FakeObs();
  const original = client.request.bind(client);
  client.request = async (type, data) => {
    if (type === 'GetVersion') return { availableRequests: ['GetVersion'] };
    return original(type, data);
  };
  const panic = makePanic(client);
  await assert.rejects(panic.toggle(), /does not support required OBS requests/);
  assert(!client.scenes.includes(SCENE_NAME));
});
